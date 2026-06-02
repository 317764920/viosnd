#include "precomp.h"

extern "C" {
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
}

#include <wdmguid.h>

#define VIOSND_MAX_BARS PCI_TYPE0_ADDRESSES
#define VIOSND_CONTROL_TIMEOUT_MS 1000u
#define VIOSND_CONTROL_POLL_DELAY_US 1000u
#define VIOSND_MAX_PCM_STREAMS 64u
#define PORT_MASK 0xFFFF

typedef struct _VIOSND_BAR {
    BOOLEAN Present;
    BOOLEAN PortSpace;
    PHYSICAL_ADDRESS BasePA;
    ULONG Length;
    PVOID BaseVA;
} VIOSND_BAR, *PVIOSND_BAR;

typedef struct _VIOSND_DMA_BLOCK {
    PVOID Va;
    SIZE_T Size;
    SINGLE_LIST_ENTRY Entry;
} VIOSND_DMA_BLOCK, *PVIOSND_DMA_BLOCK;

struct _VIOSND_DEVICE {
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    BUS_INTERFACE_STANDARD PciBus;
    BOOLEAN PciBusInterfaceValid;
    PCI_COMMON_HEADER PciHeader;
    VIOSND_BAR Bars[VIOSND_MAX_BARS];
    SINGLE_LIST_ENTRY DmaBlocks;
    VirtIODevice Vdev;
    BOOLEAN VirtioInitialized;
    struct virtqueue *Queues[VIRTIO_SND_VQ_MAX];
    KMUTEX ControlQueueMutex;
    VIRTIO_SND_CONFIG Config;
};

static NTSTATUS
ViosndPnpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

typedef struct _VIOSND_CONTROL_MESSAGE {
    PVOID RequestVa;
    PHYSICAL_ADDRESS RequestPa;
    ULONG RequestLength;
    PVOID ResponseVa;
    PHYSICAL_ADDRESS ResponsePa;
    ULONG ResponseLength;
} VIOSND_CONTROL_MESSAGE, *PVIOSND_CONTROL_MESSAGE;

struct _VIOSND_PCM_IO {
    PVOID RequestVa;
    PHYSICAL_ADDRESS RequestPa;
    ULONG RequestLength;
    VIRTIO_SND_PCM_STATUS *StatusVa;
    PHYSICAL_ADDRESS StatusPa;
    ULONG AudioLength;
};

static u8
ViosndReadByte(
    _In_ ULONG_PTR Register)
{
    if (Register & ~PORT_MASK) {
        return READ_REGISTER_UCHAR((volatile UCHAR *)Register);
    }
    return READ_PORT_UCHAR((PUCHAR)Register);
}

static u16
ViosndReadWord(
    _In_ ULONG_PTR Register)
{
    if (Register & ~PORT_MASK) {
        return READ_REGISTER_USHORT((volatile USHORT *)Register);
    }
    return READ_PORT_USHORT((PUSHORT)Register);
}

static u32
ViosndReadDword(
    _In_ ULONG_PTR Register)
{
    if (Register & ~PORT_MASK) {
        return READ_REGISTER_ULONG((volatile ULONG *)Register);
    }
    return READ_PORT_ULONG((PULONG)Register);
}

static VOID
ViosndWriteByte(
    _In_ ULONG_PTR Register,
    _In_ u8 Value)
{
    if (Register & ~PORT_MASK) {
        WRITE_REGISTER_UCHAR((volatile UCHAR *)Register, Value);
    } else {
        WRITE_PORT_UCHAR((PUCHAR)Register, Value);
    }
}

static VOID
ViosndWriteWord(
    _In_ ULONG_PTR Register,
    _In_ u16 Value)
{
    if (Register & ~PORT_MASK) {
        WRITE_REGISTER_USHORT((volatile USHORT *)Register, Value);
    } else {
        WRITE_PORT_USHORT((PUSHORT)Register, Value);
    }
}

static VOID
ViosndWriteDword(
    _In_ ULONG_PTR Register,
    _In_ u32 Value)
{
    if (Register & ~PORT_MASK) {
        WRITE_REGISTER_ULONG((volatile ULONG *)Register, Value);
    } else {
        WRITE_PORT_ULONG((PULONG)Register, Value);
    }
}

static void *
ViosndAllocContiguous(
    _In_ void *Context,
    _In_ size_t Size)
{
    PVIOSND_DEVICE device = (PVIOSND_DEVICE)Context;
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVIOSND_DMA_BLOCK block;
    SIZE_T roundedSize = ROUND_TO_PAGES(Size);

    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    boundary.QuadPart = 0;

    block = (PVIOSND_DMA_BLOCK)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                           sizeof(*block),
                                                           VIOSND_POOL_TAG);
    if (block == NULL) {
        return NULL;
    }

    block->Va = MmAllocateContiguousMemorySpecifyCache(roundedSize,
                                                       low,
                                                       high,
                                                       boundary,
                                                       MmNonCached);
    if (block->Va == NULL) {
        ExFreePoolWithTag(block, VIOSND_POOL_TAG);
        return NULL;
    }

    block->Size = roundedSize;
    RtlZeroMemory(block->Va, roundedSize);
    PushEntryList(&device->DmaBlocks, &block->Entry);
    return block->Va;
}

static VOID
ViosndFreeContiguous(
    _In_ void *Context,
    _In_ void *Virt)
{
    PVIOSND_DEVICE device = (PVIOSND_DEVICE)Context;
    PSINGLE_LIST_ENTRY previous = &device->DmaBlocks;
    PSINGLE_LIST_ENTRY current = device->DmaBlocks.Next;

    while (current != NULL) {
        PVIOSND_DMA_BLOCK block = CONTAINING_RECORD(current, VIOSND_DMA_BLOCK, Entry);
        if (block->Va == Virt) {
            previous->Next = current->Next;
            MmFreeContiguousMemory(block->Va);
            ExFreePoolWithTag(block, VIOSND_POOL_TAG);
            return;
        }
        previous = current;
        current = current->Next;
    }
}

static ULONGLONG
ViosndGetPhysicalAddress(
    _In_ void *Context,
    _In_ void *Virt)
{
    UNREFERENCED_PARAMETER(Context);
    return MmGetPhysicalAddress(Virt).QuadPart;
}

static void *
ViosndAllocNonPaged(
    _In_ void *Context,
    _In_ size_t Size)
{
    UNREFERENCED_PARAMETER(Context);
    PVOID address = ExAllocatePoolUninitialized(NonPagedPoolNx, Size, VIOSND_POOL_TAG);
    if (address != NULL) {
        RtlZeroMemory(address, Size);
    }
    return address;
}

static VOID
ViosndFreeNonPaged(
    _In_ void *Context,
    _In_ void *Address)
{
    UNREFERENCED_PARAMETER(Context);
    if (Address != NULL) {
        ExFreePoolWithTag(Address, VIOSND_POOL_TAG);
    }
}

static int
ViosndReadPciConfig(
    _In_ PVIOSND_DEVICE Device,
    _In_ int Where,
    _Out_writes_bytes_(Length) void *Buffer,
    _In_ ULONG Length)
{
    ULONG read;

    if (!Device->PciBusInterfaceValid) {
        return -1;
    }

    read = Device->PciBus.GetBusData(Device->PciBus.Context,
                                     PCI_WHICHSPACE_CONFIG,
                                     Buffer,
                                     Where,
                                     Length);
    return read == Length ? 0 : -1;
}

static int
ViosndReadConfigByte(
    _In_ void *Context,
    _In_ int Where,
    _Out_ u8 *Value)
{
    return ViosndReadPciConfig((PVIOSND_DEVICE)Context, Where, Value, sizeof(*Value));
}

static int
ViosndReadConfigWord(
    _In_ void *Context,
    _In_ int Where,
    _Out_ u16 *Value)
{
    return ViosndReadPciConfig((PVIOSND_DEVICE)Context, Where, Value, sizeof(*Value));
}

static int
ViosndReadConfigDword(
    _In_ void *Context,
    _In_ int Where,
    _Out_ u32 *Value)
{
    return ViosndReadPciConfig((PVIOSND_DEVICE)Context, Where, Value, sizeof(*Value));
}

static size_t
ViosndGetResourceLength(
    _In_ void *Context,
    _In_ int Bar)
{
    PVIOSND_DEVICE device = (PVIOSND_DEVICE)Context;

    if (Bar < 0 || Bar >= VIOSND_MAX_BARS || !device->Bars[Bar].Present) {
        return 0;
    }

    return device->Bars[Bar].Length;
}

static void *
ViosndMapAddressRange(
    _In_ void *Context,
    _In_ int Bar,
    _In_ size_t Offset,
    _In_ size_t MaxLength)
{
    PVIOSND_DEVICE device = (PVIOSND_DEVICE)Context;
    PVIOSND_BAR bar;

    UNREFERENCED_PARAMETER(MaxLength);

    if (Bar < 0 || Bar >= VIOSND_MAX_BARS || !device->Bars[Bar].Present) {
        return NULL;
    }

    bar = &device->Bars[Bar];
    if (Offset >= bar->Length) {
        return NULL;
    }

    if (bar->PortSpace) {
        return (PVOID)(ULONG_PTR)(bar->BasePA.QuadPart + Offset);
    }

    if (bar->BaseVA == NULL) {
        bar->BaseVA = MmMapIoSpaceEx(bar->BasePA,
                                     bar->Length,
                                     PAGE_READWRITE | PAGE_NOCACHE);
        if (bar->BaseVA == NULL) {
            return NULL;
        }
    }

    return (PUCHAR)bar->BaseVA + Offset;
}

static u16
ViosndGetMsixVector(
    _In_ void *Context,
    _In_ int Queue)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Queue);
    return VIRTIO_MSI_NO_VECTOR;
}

static VOID
ViosndSleep(
    _In_ void *Context,
    _In_ unsigned int Milliseconds)
{
    UNREFERENCED_PARAMETER(Context);

    if (KeGetCurrentIrql() <= APC_LEVEL) {
        LARGE_INTEGER interval;
        interval.QuadPart = Int32x32To64((LONG)Milliseconds, -10000);
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    } else {
        KeStallExecutionProcessor(Milliseconds * 1000);
    }
}

static VirtIOSystemOps ViosndSystemOps = {
    ViosndReadByte,
    ViosndReadWord,
    ViosndReadDword,
    ViosndWriteByte,
    ViosndWriteWord,
    ViosndWriteDword,
    ViosndAllocContiguous,
    ViosndFreeContiguous,
    ViosndGetPhysicalAddress,
    ViosndAllocNonPaged,
    ViosndFreeNonPaged,
    ViosndReadConfigByte,
    ViosndReadConfigWord,
    ViosndReadConfigDword,
    ViosndGetResourceLength,
    ViosndMapAddressRange,
    ViosndGetMsixVector,
    ViosndSleep
};

static NTSTATUS
ViosndSendSynchronousPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp,
                           ViosndPnpCompletion,
                           &event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(DeviceObject, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

static NTSTATUS
ViosndQueryPciBusInterface(
    _Inout_ PVIOSND_DEVICE Device)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    if (Device->PhysicalDeviceObject == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }

    irp = IoAllocateIrp(Device->PhysicalDeviceObject->StackSize, FALSE);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = (LPGUID)&GUID_BUS_INTERFACE_STANDARD;
    stack->Parameters.QueryInterface.Size = sizeof(Device->PciBus);
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&Device->PciBus;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    status = ViosndSendSynchronousPnp(Device->PhysicalDeviceObject, irp);
    IoFreeIrp(irp);

    if (NT_SUCCESS(status)) {
        Device->PciBusInterfaceValid = TRUE;
    }

    return status;
}

static NTSTATUS
ViosndReadPciHeader(
    _Inout_ PVIOSND_DEVICE Device)
{
    if (ViosndReadPciConfig(Device,
                            0,
                            &Device->PciHeader,
                            sizeof(Device->PciHeader)) != 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndMapBars(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ PRESOURCELIST ResourceList)
{
    ULONG count;

    count = ResourceList->NumberOfEntriesOfType(CmResourceTypeMemory);
    for (ULONG i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
        int barIndex;

        descriptor = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, i);
        if (descriptor == NULL) {
            continue;
        }

        barIndex = virtio_get_bar_index(&Device->PciHeader, descriptor->u.Memory.Start);
        if (barIndex < 0 || barIndex >= VIOSND_MAX_BARS) {
            continue;
        }

        Device->Bars[barIndex].Present = TRUE;
        Device->Bars[barIndex].PortSpace = FALSE;
        Device->Bars[barIndex].BasePA = descriptor->u.Memory.Start;
        Device->Bars[barIndex].Length = descriptor->u.Memory.Length;
    }

    count = ResourceList->NumberOfEntriesOfType(CmResourceTypePort);
    for (ULONG i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
        int barIndex;

        descriptor = ResourceList->FindTranslatedEntry(CmResourceTypePort, i);
        if (descriptor == NULL) {
            continue;
        }

        barIndex = virtio_get_bar_index(&Device->PciHeader, descriptor->u.Port.Start);
        if (barIndex < 0 || barIndex >= VIOSND_MAX_BARS) {
            continue;
        }

        Device->Bars[barIndex].Present = TRUE;
        Device->Bars[barIndex].PortSpace = TRUE;
        Device->Bars[barIndex].BasePA = descriptor->u.Port.Start;
        Device->Bars[barIndex].Length = descriptor->u.Port.Length;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndInitVirtio(
    _Inout_ PVIOSND_DEVICE Device)
{
    NTSTATUS status;
    u64 features;

    status = virtio_device_initialize(&Device->Vdev, &ViosndSystemOps, Device, false);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    virtio_device_reset(&Device->Vdev);
    virtio_add_status(&Device->Vdev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    virtio_add_status(&Device->Vdev, VIRTIO_CONFIG_S_DRIVER);

    features = virtio_get_features(&Device->Vdev);
    features &= (1ULL << VIRTIO_F_VERSION_1);

    status = virtio_set_features(&Device->Vdev, features);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = virtio_find_queues(&Device->Vdev, VIRTIO_SND_VQ_MAX, Device->Queues);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    virtio_get_config(&Device->Vdev, 0, &Device->Config, sizeof(Device->Config));
    virtio_device_ready(&Device->Vdev);
    Device->VirtioInitialized = TRUE;

    return STATUS_SUCCESS;
}

static PVOID
ViosndAllocControlBuffer(
    _In_ ULONG Size,
    _Out_ PPHYSICAL_ADDRESS PhysicalAddress)
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVOID buffer;

    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    boundary.QuadPart = 0;

    buffer = MmAllocateContiguousMemorySpecifyCache(ROUND_TO_PAGES(Size),
                                                   low,
                                                   high,
                                                   boundary,
                                                   MmNonCached);
    if (buffer != NULL) {
        RtlZeroMemory(buffer, ROUND_TO_PAGES(Size));
        *PhysicalAddress = MmGetPhysicalAddress(buffer);
    }

    return buffer;
}

static VOID
ViosndFreeControlBuffer(
    _In_opt_ PVOID Buffer)
{
    if (Buffer != NULL) {
        MmFreeContiguousMemory(Buffer);
    }
}

static VOID
ViosndFreeControlMessage(
    _In_opt_ PVIOSND_CONTROL_MESSAGE Message)
{
    if (Message == NULL) {
        return;
    }

    ViosndFreeControlBuffer(Message->RequestVa);
    ViosndFreeControlBuffer(Message->ResponseVa);
    ExFreePoolWithTag(Message, VIOSND_POOL_TAG);
}

static NTSTATUS
ViosndControlCommand(
    _Inout_ PVIOSND_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Out_writes_bytes_(ResponseLength) VOID *Response,
    _In_ ULONG ResponseLength)
{
    PVIOSND_CONTROL_MESSAGE message;
    VirtIOBufferDescriptor sg[2];
    ULONG elapsedUs = 0;
    unsigned int usedLength = 0;
    PVOID used;
    int result;
    BOOLEAN lockHeld = FALSE;
    BOOLEAN queued = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device->Queues[VIRTIO_SND_VQ_CONTROL] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    message = (PVIOSND_CONTROL_MESSAGE)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                   sizeof(*message),
                                                                   VIOSND_POOL_TAG);
    if (message == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(message, sizeof(*message));
    message->RequestVa = ViosndAllocControlBuffer(RequestLength, &message->RequestPa);
    message->ResponseVa = ViosndAllocControlBuffer(ResponseLength, &message->ResponsePa);
    if (message->RequestVa == NULL || message->ResponseVa == NULL) {
        ViosndFreeControlMessage(message);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    message->RequestLength = RequestLength;
    message->ResponseLength = ResponseLength;
    RtlCopyMemory(message->RequestVa, Request, RequestLength);

    sg[0].physAddr = message->RequestPa;
    sg[0].length = RequestLength;
    sg[1].physAddr = message->ResponsePa;
    sg[1].length = ResponseLength;

    KeWaitForSingleObject(&Device->ControlQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
    lockHeld = TRUE;

    result = virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_CONTROL],
                               sg,
                               1,
                               1,
                               message,
                               NULL,
                               0);
    if (result < 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }
    queued = TRUE;

    virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_CONTROL]);

    do {
        used = virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_CONTROL], &usedLength);
        if (used != NULL) {
            PVIOSND_CONTROL_MESSAGE completed = (PVIOSND_CONTROL_MESSAGE)used;

            if (completed != message) {
                ViosndFreeControlMessage(completed);
                continue;
            }

            RtlCopyMemory(Response, message->ResponseVa, ResponseLength);
            status = STATUS_SUCCESS;
            ViosndFreeControlMessage(message);
            message = NULL;
            goto Exit;
        }

        if (KeGetCurrentIrql() <= APC_LEVEL) {
            LARGE_INTEGER interval;

            interval.QuadPart = -(10LL * VIOSND_CONTROL_POLL_DELAY_US);
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
        } else {
            KeStallExecutionProcessor(VIOSND_CONTROL_POLL_DELAY_US);
        }
        elapsedUs += VIOSND_CONTROL_POLL_DELAY_US;
    } while (elapsedUs < VIOSND_CONTROL_TIMEOUT_MS * 1000u);

    status = STATUS_IO_TIMEOUT;

Exit:
    if (lockHeld) {
        KeReleaseMutex(&Device->ControlQueueMutex, FALSE);
    }
    if (message != NULL && (!queued || status != STATUS_IO_TIMEOUT)) {
        ViosndFreeControlMessage(message);
    }
    return status;
}

static NTSTATUS
ViosndControlStatusCommand(
    _Inout_ PVIOSND_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength)
{
    VIRTIO_SND_HDR response;
    NTSTATUS status;

    status = ViosndControlCommand(Device, Request, RequestLength, &response, sizeof(response));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return response.code == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static NTSTATUS
ViosndPcmCommand(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ ULONG Command)
{
    VIRTIO_SND_PCM_HDR request;
    NTSTATUS status;

    RtlZeroMemory(&request, sizeof(request));
    request.hdr.code = Command;
    request.stream_id = StreamId;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: pcm command begin stream=%u command=0x%x\n",
               StreamId,
               Command);
    status = ViosndControlStatusCommand(Device, &request, sizeof(request));
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: pcm command done stream=%u command=0x%x status=0x%08x\n",
               StreamId,
               Command,
               status);
    return status;
}

static NTSTATUS
ViosndWaitForUsedBuffer(
    _In_ struct virtqueue *Queue,
    _In_ PVOID Opaque,
    _Out_opt_ PULONG Length)
{
    ULONG elapsedUs = 0;
    unsigned int usedLength;
    LARGE_INTEGER interval;

    interval.QuadPart = -(10LL * 1000LL);

    do {
        PVOID used = virtqueue_get_buf(Queue, &usedLength);
        if (used == Opaque) {
            if (Length != NULL) {
                *Length = usedLength;
            }
            return STATUS_SUCCESS;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        elapsedUs += VIOSND_CONTROL_POLL_DELAY_US;
    } while (elapsedUs < VIOSND_CONTROL_TIMEOUT_MS * 1000u);

    return STATUS_IO_TIMEOUT;
}

NTSTATUS
ViosndCreateDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ PRESOURCELIST ResourceList,
    _Outptr_ PVIOSND_DEVICE *Device)
{
    PVIOSND_DEVICE device;
    NTSTATUS status;

    *Device = NULL;
    device = (PVIOSND_DEVICE)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                        sizeof(*device),
                                                        VIOSND_POOL_TAG);
    if (device == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(device, sizeof(*device));
    KeInitializeMutex(&device->ControlQueueMutex, 0);
    device->DeviceObject = DeviceObject;
    device->PhysicalDeviceObject = PhysicalDeviceObject;
    if (device->PhysicalDeviceObject != NULL) {
        ObReferenceObject(device->PhysicalDeviceObject);
    }

    status = ViosndQueryPciBusInterface(device);
    if (!NT_SUCCESS(status)) {
        ViosndDestroyDevice(device);
        return status;
    }

    status = ViosndReadPciHeader(device);
    if (!NT_SUCCESS(status)) {
        ViosndDestroyDevice(device);
        return status;
    }

    status = ViosndMapBars(device, ResourceList);
    if (!NT_SUCCESS(status)) {
        ViosndDestroyDevice(device);
        return status;
    }

    *Device = device;
    return STATUS_SUCCESS;
}

VOID
ViosndDestroyDevice(
    _In_opt_ PVIOSND_DEVICE Device)
{
    if (Device == NULL) {
        return;
    }

    if (Device->VirtioInitialized) {
        virtio_device_reset(&Device->Vdev);
        virtio_delete_queues(&Device->Vdev);
        virtio_device_shutdown(&Device->Vdev);
    }

    for (ULONG i = 0; i < VIOSND_MAX_BARS; ++i) {
        if (Device->Bars[i].BaseVA != NULL) {
            MmUnmapIoSpace(Device->Bars[i].BaseVA, Device->Bars[i].Length);
        }
    }

    while (Device->DmaBlocks.Next != NULL) {
        PSINGLE_LIST_ENTRY entry = PopEntryList(&Device->DmaBlocks);
        PVIOSND_DMA_BLOCK block = CONTAINING_RECORD(entry, VIOSND_DMA_BLOCK, Entry);
        MmFreeContiguousMemory(block->Va);
        ExFreePoolWithTag(block, VIOSND_POOL_TAG);
    }

    if (Device->PciBusInterfaceValid && Device->PciBus.InterfaceDereference != NULL) {
        Device->PciBus.InterfaceDereference(Device->PciBus.Context);
    }

    if (Device->PhysicalDeviceObject != NULL) {
        ObDereferenceObject(Device->PhysicalDeviceObject);
    }

    ExFreePoolWithTag(Device, VIOSND_POOL_TAG);
}

NTSTATUS
ViosndInitializeDevice(
    _Inout_ PVIOSND_DEVICE Device)
{
    NTSTATUS status = ViosndInitVirtio(Device);

    if (NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: config jacks=%u streams=%u chmaps=%u controls=%u\n",
                   Device->Config.jacks,
                   Device->Config.streams,
                   Device->Config.chmaps,
                   Device->Config.controls);
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ViosndInitVirtio failed 0x%08x\n",
                   status);
    }

    return status;
}

NTSTATUS
ViosndQueryPcmStreams(
    _Inout_ PVIOSND_DEVICE Device,
    _Out_ PVIOSND_STREAM_PAIR Pair)
{
    VIRTIO_SND_QUERY_INFO query;
    PUCHAR response;
    VIRTIO_SND_HDR responseHeader;
    VIRTIO_SND_PCM_INFO info[VIOSND_MAX_PCM_STREAMS];
    ULONG streamCount;
    ULONG responseLength;
    NTSTATUS status;

    streamCount = min(Device->Config.streams, VIOSND_MAX_PCM_STREAMS);
    if (streamCount == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: device reports zero PCM streams\n");
        return STATUS_NOT_FOUND;
    }

    responseLength = sizeof(VIRTIO_SND_HDR) + streamCount * sizeof(VIRTIO_SND_PCM_INFO);
    response = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                   responseLength,
                                                   VIOSND_POOL_TAG);
    if (response == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&query, sizeof(query));
    query.hdr.code = VIRTIO_SND_R_PCM_INFO;
    query.start_id = 0;
    query.count = streamCount;
    query.size = sizeof(VIRTIO_SND_PCM_INFO);

    status = ViosndControlCommand(Device,
                                  &query,
                                  sizeof(query),
                                  response,
                                  responseLength);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(&responseHeader, response, sizeof(responseHeader));
        RtlCopyMemory(info,
                      response + sizeof(VIRTIO_SND_HDR),
                      streamCount * sizeof(VIRTIO_SND_PCM_INFO));
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PCM_INFO status=0x%08x count=%u\n",
                   responseHeader.code,
                   streamCount);
        for (ULONG i = 0; i < streamCount; ++i) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream[%u] dir=%u ch=%u-%u formats=0x%llx rates=0x%llx features=0x%x\n",
                       i,
                       info[i].direction,
                       info[i].channels_min,
                       info[i].channels_max,
                       info[i].formats,
                       info[i].rates,
                       info[i].features);
        }
        status = responseHeader.code == VIRTIO_SND_S_OK
                     ? ViosndFindStreamPair(info, streamCount, Pair)
                     : STATUS_NOT_SUPPORTED;
        if (NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: selected render=%u/%u capture=%u/%u\n",
                       Pair->HasRender,
                       Pair->RenderStreamId,
                       Pair->HasCapture,
                       Pair->CaptureStreamId);
        }
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PCM_INFO command failed 0x%08x\n",
                   status);
    }

    ExFreePoolWithTag(response, VIOSND_POOL_TAG);
    return status;
}

NTSTATUS
ViosndConfigureDefaultPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    VIOSND_PCM_FORMAT format;
    VIRTIO_SND_PCM_SET_PARAMS params;
    VIRTIO_SND_PCM_HDR command;
    NTSTATUS status;

    ViosndGetDefaultPcmFormat(&format);
    ViosndBuildSetParams(&params, StreamId, &format);

    status = ViosndControlStatusCommand(Device, &params, sizeof(params));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&command, sizeof(command));
    command.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    command.stream_id = StreamId;
    return ViosndControlStatusCommand(Device, &command, sizeof(command));
}

NTSTATUS
ViosndStartPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    return ViosndPcmCommand(Device, StreamId, VIRTIO_SND_R_PCM_START);
}

NTSTATUS
ViosndStopPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    return ViosndPcmCommand(Device, StreamId, VIRTIO_SND_R_PCM_STOP);
}

NTSTATUS
ViosndReleasePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    return ViosndPcmCommand(Device, StreamId, VIRTIO_SND_R_PCM_RELEASE);
}

NTSTATUS
ViosndWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG BytesWritten)
{
    struct _WRITE_MESSAGE {
        VIRTIO_SND_PCM_XFER Header;
    } *request;
    VIRTIO_SND_PCM_STATUS *status;
    PHYSICAL_ADDRESS requestPa;
    PHYSICAL_ADDRESS statusPa;
    VirtIOBufferDescriptor sg[2];
    ULONG requestLength;
    NTSTATUS result;

    if (BytesWritten != NULL) {
        *BytesWritten = 0;
    }

    if (Device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    requestLength = sizeof(*request) + Length;
    request = (struct _WRITE_MESSAGE *)ViosndAllocControlBuffer(requestLength, &requestPa);
    status = (VIRTIO_SND_PCM_STATUS *)ViosndAllocControlBuffer(sizeof(*status), &statusPa);
    if (request == NULL || status == NULL) {
        ViosndFreeControlBuffer(request);
        ViosndFreeControlBuffer(status);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    request->Header.stream_id = StreamId;
    RtlCopyMemory((PUCHAR)request + sizeof(*request), Buffer, Length);

    sg[0].physAddr = requestPa;
    sg[0].length = requestLength;
    sg[1].physAddr = statusPa;
    sg[1].length = sizeof(*status);

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_TX], sg, 1, 1, request, NULL, 0) < 0) {
        result = STATUS_DEVICE_BUSY;
    } else {
        virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_TX]);
        result = ViosndWaitForUsedBuffer(Device->Queues[VIRTIO_SND_VQ_TX], request, NULL);
        if (NT_SUCCESS(result)) {
            result = status->status == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
            if (NT_SUCCESS(result) && BytesWritten != NULL) {
                *BytesWritten = Length;
            }
        }
    }

    ViosndFreeControlBuffer(request);
    ViosndFreeControlBuffer(status);
    return result;
}

NTSTATUS
ViosndAllocateWritePcmIo(
    _In_ ULONG MaxAudioLength,
    _Outptr_ PVIOSND_PCM_IO *Io)
{
    PVIOSND_PCM_IO io;

    *Io = NULL;
    io = (PVIOSND_PCM_IO)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                     sizeof(*io),
                                                     VIOSND_POOL_TAG);
    if (io == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(io, sizeof(*io));

    io->RequestLength = sizeof(VIRTIO_SND_PCM_XFER) + MaxAudioLength;
    io->RequestVa = ViosndAllocControlBuffer(io->RequestLength, &io->RequestPa);
    io->StatusVa = (VIRTIO_SND_PCM_STATUS *)ViosndAllocControlBuffer(sizeof(*io->StatusVa),
                                                                     &io->StatusPa);
    if (io->RequestVa == NULL || io->StatusVa == NULL) {
        ViosndFreeControlBuffer(io->RequestVa);
        ViosndFreeControlBuffer(io->StatusVa);
        ExFreePoolWithTag(io, VIOSND_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Io = io;
    return STATUS_SUCCESS;
}

VOID
ViosndFreeWritePcmIo(
    _In_opt_ PVIOSND_PCM_IO Io)
{
    if (Io == NULL) {
        return;
    }

    ViosndFreeControlBuffer(Io->RequestVa);
    ViosndFreeControlBuffer(Io->StatusVa);
    ExFreePoolWithTag(Io, VIOSND_POOL_TAG);
}

NTSTATUS
ViosndSubmitPreparedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Inout_ PVIOSND_PCM_IO Io)
{
    VIRTIO_SND_PCM_XFER *header;
    VirtIOBufferDescriptor sg[2];
    ULONG requestLength;

    if (Device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (Io == NULL ||
        Io->RequestVa == NULL ||
        Io->StatusVa == NULL ||
        Io->RequestLength < sizeof(VIRTIO_SND_PCM_XFER) + Length) {
        return STATUS_INVALID_PARAMETER;
    }

    requestLength = sizeof(VIRTIO_SND_PCM_XFER) + Length;
    RtlZeroMemory(Io->StatusVa, sizeof(*Io->StatusVa));
    header = (VIRTIO_SND_PCM_XFER *)Io->RequestVa;
    header->stream_id = StreamId;
    RtlCopyMemory((PUCHAR)Io->RequestVa + sizeof(*header), Buffer, Length);
    Io->AudioLength = Length;

    sg[0].physAddr = Io->RequestPa;
    sg[0].length = requestLength;
    sg[1].physAddr = Io->StatusPa;
    sg[1].length = sizeof(*Io->StatusVa);

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_TX], sg, 1, 1, Io, NULL, 0) < 0) {
        return STATUS_DEVICE_BUSY;
    }

    virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_TX]);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndSubmitWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    PVIOSND_PCM_IO io;
    NTSTATUS status;

    status = ViosndAllocateWritePcmIo(Length, &io);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ViosndSubmitPreparedWritePcm(Device, StreamId, Buffer, Length, io);
    if (!NT_SUCCESS(status)) {
        ViosndFreeWritePcmIo(io);
    }
    return status;
}

NTSTATUS
ViosndReclaimPreparedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Outptr_opt_result_maybenull_ PVIOSND_PCM_IO *Io,
    _Out_opt_ PULONG BytesWritten,
    _Out_opt_ PULONG LatencyBytes)
{
    unsigned int usedLength;
    PVIOSND_PCM_IO io;
    NTSTATUS status;

    if (Io != NULL) {
        *Io = NULL;
    }
    if (BytesWritten != NULL) {
        *BytesWritten = 0;
    }
    if (LatencyBytes != NULL) {
        *LatencyBytes = 0;
    }

    if (Device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    io = (PVIOSND_PCM_IO)virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_TX], &usedLength);
    if (io == NULL) {
        return STATUS_NOT_FOUND;
    }

    UNREFERENCED_PARAMETER(usedLength);
    status = io->StatusVa->status == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    if (LatencyBytes != NULL) {
        *LatencyBytes = io->StatusVa->latency_bytes;
    }
    if (NT_SUCCESS(status) && BytesWritten != NULL) {
        *BytesWritten = io->AudioLength;
    }
    if (Io != NULL) {
        *Io = io;
    }
    return status;
}

NTSTATUS
ViosndReclaimWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Out_opt_ PULONG BytesWritten)
{
    PVIOSND_PCM_IO io;
    NTSTATUS status;

    status = ViosndReclaimPreparedWritePcm(Device, &io, BytesWritten, NULL);
    if (status != STATUS_NOT_FOUND) {
        ViosndFreeWritePcmIo(io);
    }
    return status;
}

NTSTATUS
ViosndReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _Out_writes_bytes_(Length) VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG BytesRead)
{
    VIRTIO_SND_PCM_XFER *request;
    PVOID response;
    VIRTIO_SND_PCM_STATUS status;
    PHYSICAL_ADDRESS requestPa;
    PHYSICAL_ADDRESS responsePa;
    VirtIOBufferDescriptor sg[2];
    ULONG responseLength;
    NTSTATUS result;
    ULONG usedLength = 0;

    if (BytesRead != NULL) {
        *BytesRead = 0;
    }

    if (Device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    responseLength = Length + sizeof(VIRTIO_SND_PCM_STATUS);
    request = (VIRTIO_SND_PCM_XFER *)ViosndAllocControlBuffer(sizeof(*request), &requestPa);
    response = ViosndAllocControlBuffer(responseLength, &responsePa);
    if (request == NULL || response == NULL) {
        ViosndFreeControlBuffer(request);
        ViosndFreeControlBuffer(response);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    request->stream_id = StreamId;
    sg[0].physAddr = requestPa;
    sg[0].length = sizeof(*request);
    sg[1].physAddr = responsePa;
    sg[1].length = responseLength;

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_RX], sg, 1, 1, request, NULL, 0) < 0) {
        result = STATUS_DEVICE_BUSY;
    } else {
        virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_RX]);
        result = ViosndWaitForUsedBuffer(Device->Queues[VIRTIO_SND_VQ_RX], request, &usedLength);
        if (NT_SUCCESS(result)) {
            if (usedLength < sizeof(status) || usedLength > responseLength) {
                result = STATUS_IO_DEVICE_ERROR;
            } else {
                ULONG audioBytes = usedLength - sizeof(status);
                RtlCopyMemory(&status, (PUCHAR)response + audioBytes, sizeof(status));
                result = status.status == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
                if (NT_SUCCESS(result)) {
                    RtlCopyMemory(Buffer, response, min(audioBytes, Length));
                    if (BytesRead != NULL) {
                        *BytesRead = min(audioBytes, Length);
                    }
                }
            }
        }
    }

    if (!NT_SUCCESS(result)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: read pcm stream=%u length=%u failed 0x%08x used=%u\n",
                   StreamId,
                   Length,
                   result,
                   usedLength);
    }

    ViosndFreeControlBuffer(request);
    ViosndFreeControlBuffer(response);
    return result;
}
