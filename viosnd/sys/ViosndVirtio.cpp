#include "precomp.h"

extern "C" {
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
}

#include <wdmguid.h>

#define VIOSND_MAX_BARS PCI_TYPE0_ADDRESSES
#define VIOSND_CONTROL_TIMEOUT_MS 5000u
#define VIOSND_CONTROL_POLL_DELAY_US 1000u
#define VIOSND_CONTROL_RENOTIFY_US 100000u
#define VIOSND_CONTROL_STATUS_RETRY_COUNT 4u
#define VIOSND_CONTROL_STATUS_RETRY_DELAY_MS 5u
#define VIOSND_PCM_BAD_MSG_RETRY_COUNT 4u
#define VIOSND_PCM_BAD_MSG_RETRY_DELAY_MS 10u
#define VIOSND_MAX_PCM_STREAMS 64u
#define VIOSND_EVENT_BUFFER_COUNT 8u
#define VIOSND_EVENT_POLL_BUDGET 8u
#define VIOSND_PCM_INFO_RETRY_COUNT 8u
#define VIOSND_PCM_INFO_RETRY_DELAY_MS 100u
#define VIOSND_TX_MUTEX_TIMEOUT_MS 100u
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
    PHYSICAL_ADDRESS LogicalAddress;
    SIZE_T Size;
    SINGLE_LIST_ENTRY Entry;
} VIOSND_DMA_BLOCK, *PVIOSND_DMA_BLOCK;

typedef struct _VIOSND_EVENT_BUFFER {
    VIRTIO_SND_EVENT *EventVa;
    PHYSICAL_ADDRESS EventPa;
} VIOSND_EVENT_BUFFER, *PVIOSND_EVENT_BUFFER;

struct _VIOSND_DEVICE {
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    BUS_INTERFACE_STANDARD PciBus;
    BOOLEAN PciBusInterfaceValid;
    PCI_COMMON_HEADER PciHeader;
    VIOSND_BAR Bars[VIOSND_MAX_BARS];
    SINGLE_LIST_ENTRY DmaBlocks;
    PDMA_ADAPTER DmaAdapter;
    ULONG DmaMapRegisters;
    VirtIODevice Vdev;
    BOOLEAN VirtioInitialized;
    struct virtqueue *Queues[VIRTIO_SND_VQ_MAX];
    KMUTEX ControlQueueMutex;
    KMUTEX TxQueueMutex;
    KMUTEX RxQueueMutex;
    KMUTEX EventQueueMutex;
    VIRTIO_SND_CONFIG Config;
    VIOSND_EVENT_BUFFER EventBuffers[VIOSND_EVENT_BUFFER_COUNT];
    ULONG EventBufferCount;
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
    PVOID AudioVa;
    PHYSICAL_ADDRESS AudioPa;
    ULONG MaxAudioLength;
    VIRTIO_SND_PCM_STATUS *StatusVa;
    PHYSICAL_ADDRESS StatusPa;
    ULONG AudioLength;
    ULONG SourceLength;
    ULONG PacketNumber;
    ULONG StreamId;
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
ViosndAllocateDmaBuffer(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ size_t Size,
    _Out_ PPHYSICAL_ADDRESS LogicalAddress)
{
    PVIOSND_DMA_BLOCK block;
    SIZE_T roundedSize = ROUND_TO_PAGES(Size);

    LogicalAddress->QuadPart = 0;
    if (Device->DmaAdapter == NULL || roundedSize > MAXULONG) {
        return NULL;
    }

    block = (PVIOSND_DMA_BLOCK)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                           sizeof(*block),
                                                           VIOSND_POOL_TAG);
    if (block == NULL) {
        return NULL;
    }
    RtlZeroMemory(block, sizeof(*block));

    block->Va = Device->DmaAdapter->DmaOperations->AllocateCommonBuffer(Device->DmaAdapter,
                                                                        (ULONG)roundedSize,
                                                                        &block->LogicalAddress,
                                                                        FALSE);
    if (block->Va == NULL) {
        ExFreePoolWithTag(block, VIOSND_POOL_TAG);
        return NULL;
    }

    block->Size = roundedSize;
    RtlZeroMemory(block->Va, roundedSize);
    PushEntryList(&Device->DmaBlocks, &block->Entry);
    *LogicalAddress = block->LogicalAddress;
    return block->Va;
}

static VOID
ViosndFreeDmaBuffer(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ void *Virt)
{
    PSINGLE_LIST_ENTRY previous;
    PSINGLE_LIST_ENTRY current;

    if (Virt == NULL) {
        return;
    }

    previous = &Device->DmaBlocks;
    current = Device->DmaBlocks.Next;

    while (current != NULL) {
        PVIOSND_DMA_BLOCK block = CONTAINING_RECORD(current, VIOSND_DMA_BLOCK, Entry);
        if (block->Va == Virt) {
            previous->Next = current->Next;
            if (Device->DmaAdapter != NULL) {
                Device->DmaAdapter->DmaOperations->FreeCommonBuffer(Device->DmaAdapter,
                                                                    (ULONG)block->Size,
                                                                    block->LogicalAddress,
                                                                    block->Va,
                                                                    FALSE);
            }
            ExFreePoolWithTag(block, VIOSND_POOL_TAG);
            return;
        }
        previous = current;
        current = current->Next;
    }
}

static ULONGLONG
ViosndGetDmaAddress(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ void *Virt)
{
    ULONG_PTR va = (ULONG_PTR)Virt;
    PSINGLE_LIST_ENTRY current = Device->DmaBlocks.Next;

    while (current != NULL) {
        PVIOSND_DMA_BLOCK block = CONTAINING_RECORD(current, VIOSND_DMA_BLOCK, Entry);
        ULONG_PTR start = (ULONG_PTR)block->Va;
        ULONG_PTR end = start + block->Size;

        if (va >= start && va < end) {
            return block->LogicalAddress.QuadPart + (va - start);
        }
        current = current->Next;
    }

    return 0;
}

static void *
ViosndAllocContiguous(
    _In_ void *Context,
    _In_ size_t Size)
{
    PHYSICAL_ADDRESS logicalAddress;

    return ViosndAllocateDmaBuffer((PVIOSND_DEVICE)Context, Size, &logicalAddress);
}

static VOID
ViosndFreeContiguous(
    _In_ void *Context,
    _In_ void *Virt)
{
    ViosndFreeDmaBuffer((PVIOSND_DEVICE)Context, Virt);
}

static ULONGLONG
ViosndGetPhysicalAddress(
    _In_ void *Context,
    _In_ void *Virt)
{
    return ViosndGetDmaAddress((PVIOSND_DEVICE)Context, Virt);
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

static VOID
ViosndFreeControlBuffer(
    _Inout_ PVIOSND_DEVICE Device,
    _In_opt_ PVOID Buffer);

static NTSTATUS
ViosndInitializeEventQueue(
    _Inout_ PVIOSND_DEVICE Device);

static VOID
ViosndKickPcmQueue(
    _Inout_ struct virtqueue *Queue)
{
#if defined(_ARM64_) || defined(ARM64)
    virtqueue_kick_always(Queue);
#else
    virtqueue_kick(Queue);
#endif
}

static VOID
ViosndPublishDmaWrites()
{
    KeMemoryBarrier();
#if defined(_ARM64_) || defined(ARM64)
    __dmb(_ARM64_BARRIER_SY);
    KeMemoryBarrier();
#endif
}

static VOID
ViosndDelayMilliseconds(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -(10LL * 1000LL * Milliseconds);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static NTSTATUS
ViosndAcquireTxQueue(
    _Inout_ PVIOSND_DEVICE Device,
    _In_z_ PCSTR Operation)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;

    timeout.QuadPart = -(10LL * 1000LL * VIOSND_TX_MUTEX_TIMEOUT_MS);
    status = KeWaitForSingleObject(&Device->TxQueueMutex,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   &timeout);
    if (status == STATUS_TIMEOUT) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: tx queue mutex timeout op=%s\n",
                   Operation);
        return STATUS_IO_TIMEOUT;
    }

    return status;
}

VOID
ViosndKickTxQueue(
    _Inout_ PVIOSND_DEVICE Device)
{
    if (Device == NULL ||
        Device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return;
    }

    if (!NT_SUCCESS(ViosndAcquireTxQueue(Device, "kick"))) {
        return;
    }
    ViosndPublishDmaWrites();
    virtqueue_kick_always(Device->Queues[VIRTIO_SND_VQ_TX]);
    KeReleaseMutex(&Device->TxQueueMutex, FALSE);
}

static BOOLEAN
ViosndIsPcmStatusCommand(
    _In_ ULONG Command)
{
    switch (Command) {
    case VIRTIO_SND_R_PCM_SET_PARAMS:
    case VIRTIO_SND_R_PCM_PREPARE:
    case VIRTIO_SND_R_PCM_START:
    case VIRTIO_SND_R_PCM_STOP:
    case VIRTIO_SND_R_PCM_RELEASE:
        return TRUE;
    default:
        return FALSE;
    }
}

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
ViosndInitializeDmaAdapter(
    _Inout_ PVIOSND_DEVICE Device)
{
    DEVICE_DESCRIPTION description;

    if (Device->PhysicalDeviceObject == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }

    RtlZeroMemory(&description, sizeof(description));
    description.Version = DEVICE_DESCRIPTION_VERSION;
    description.Master = TRUE;
    description.ScatterGather = TRUE;
    description.Dma32BitAddresses = FALSE;
    description.Dma64BitAddresses = TRUE;
    description.InterfaceType = PCIBus;
    description.DmaWidth = Width64Bits;
    description.DmaSpeed = Compatible;
    description.MaximumLength = 0x0FFFFFFF;

    Device->DmaAdapter = IoGetDmaAdapter(Device->PhysicalDeviceObject,
                                         &description,
                                         &Device->DmaMapRegisters);
    if (Device->DmaAdapter == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: DMA adapter initialized mapRegisters=%u\n",
               Device->DmaMapRegisters);
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
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: host features=0x%llx\n",
               features);
    features &= (1ULL << VIRTIO_F_VERSION_1);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: driver features=0x%llx\n",
               features);

    status = virtio_set_features(&Device->Vdev, features);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    status = virtio_find_queues(&Device->Vdev, VIRTIO_SND_VQ_MAX, Device->Queues);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    virtio_device_ready(&Device->Vdev);
    Device->VirtioInitialized = TRUE;

    virtio_get_config(&Device->Vdev, 0, &Device->Config, sizeof(Device->Config));

    return STATUS_SUCCESS;

Fail:
    for (ULONG i = 0; i < VIOSND_EVENT_BUFFER_COUNT; ++i) {
        ViosndFreeControlBuffer(Device, Device->EventBuffers[i].EventVa);
        Device->EventBuffers[i].EventVa = NULL;
    }
    Device->EventBufferCount = 0;
    virtio_device_reset(&Device->Vdev);
    virtio_delete_queues(&Device->Vdev);
    virtio_device_shutdown(&Device->Vdev);
    return status;
}

static PVOID
ViosndAllocControlBuffer(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG Size,
    _Out_ PPHYSICAL_ADDRESS LogicalAddress)
{
    return ViosndAllocateDmaBuffer(Device, Size, LogicalAddress);
}

static VOID
ViosndFreeControlBuffer(
    _Inout_ PVIOSND_DEVICE Device,
    _In_opt_ PVOID Buffer)
{
    ViosndFreeDmaBuffer(Device, Buffer);
}

static NTSTATUS
ViosndPostEventBufferLocked(
    _Inout_ PVIOSND_DEVICE Device,
    _Inout_ PVIOSND_EVENT_BUFFER Buffer)
{
    VirtIOBufferDescriptor sg;

    if (Device->Queues[VIRTIO_SND_VQ_EVENT] == NULL ||
        Buffer->EventVa == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    RtlZeroMemory(Buffer->EventVa, sizeof(*Buffer->EventVa));
    sg.physAddr = Buffer->EventPa;
    sg.length = sizeof(*Buffer->EventVa);

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_EVENT],
                          &sg,
                          0,
                          1,
                          Buffer,
                          NULL,
                          0) < 0) {
        return STATUS_DEVICE_BUSY;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndInitializeEventQueue(
    _Inout_ PVIOSND_DEVICE Device)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Device->Queues[VIRTIO_SND_VQ_EVENT] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    KeWaitForSingleObject(&Device->EventQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    for (ULONG i = 0; i < VIOSND_EVENT_BUFFER_COUNT; ++i) {
        Device->EventBuffers[i].EventVa =
            (VIRTIO_SND_EVENT *)ViosndAllocControlBuffer(Device,
                                                         sizeof(VIRTIO_SND_EVENT),
                                                         &Device->EventBuffers[i].EventPa);
        if (Device->EventBuffers[i].EventVa == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        status = ViosndPostEventBufferLocked(Device, &Device->EventBuffers[i]);
        if (!NT_SUCCESS(status)) {
            break;
        }
        Device->EventBufferCount++;
    }

    if (NT_SUCCESS(status)) {
        virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_EVENT]);
    }

    KeReleaseMutex(&Device->EventQueueMutex, FALSE);
    return status;
}

VOID
ViosndPollEvents(
    _Inout_ PVIOSND_DEVICE Device)
{
    unsigned int usedLength;
    ULONG processed = 0;

    if (Device == NULL ||
        Device->Queues[VIRTIO_SND_VQ_EVENT] == NULL) {
        return;
    }

    KeWaitForSingleObject(&Device->EventQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    for (;;) {
        PVIOSND_EVENT_BUFFER buffer;

        if (processed >= VIOSND_EVENT_POLL_BUDGET) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_WARNING_LEVEL,
                       "viosnd: event poll budget exhausted count=%u\n",
                       processed);
            break;
        }

        buffer = (PVIOSND_EVENT_BUFFER)virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_EVENT],
                                                        &usedLength);
        if (buffer == NULL) {
            break;
        }
        processed++;

        KeMemoryBarrier();
        if (buffer->EventVa != NULL && usedLength >= sizeof(VIRTIO_SND_EVENT)) {
            switch (buffer->EventVa->hdr.code) {
            case VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED:
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_TRACE_LEVEL,
                           "viosnd: event PERIOD_ELAPSED stream=%u\n",
                           buffer->EventVa->data);
                break;
            case VIRTIO_SND_EVT_PCM_XRUN:
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: event XRUN stream=%u\n",
                           buffer->EventVa->data);
                break;
            case VIRTIO_SND_EVT_JACK_CONNECTED:
            case VIRTIO_SND_EVT_JACK_DISCONNECTED:
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_TRACE_LEVEL,
                           "viosnd: event jack code=0x%x data=%u\n",
                           buffer->EventVa->hdr.code,
                           buffer->EventVa->data);
                break;
            default:
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_WARNING_LEVEL,
                           "viosnd: unexpected event code=0x%x data=%u length=%u\n",
                           buffer->EventVa->hdr.code,
                           buffer->EventVa->data,
                           usedLength);
                break;
            }
        }

        if (NT_SUCCESS(ViosndPostEventBufferLocked(Device, buffer))) {
            virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_EVENT]);
        }
    }

    KeReleaseMutex(&Device->EventQueueMutex, FALSE);
}

static VOID
ViosndFreeControlMessage(
    _Inout_ PVIOSND_DEVICE Device,
    _In_opt_ PVIOSND_CONTROL_MESSAGE Message)
{
    if (Message == NULL) {
        return;
    }

    ViosndFreeControlBuffer(Device, Message->RequestVa);
    ViosndFreeControlBuffer(Device, Message->ResponseVa);
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
    message->RequestVa = ViosndAllocControlBuffer(Device, RequestLength, &message->RequestPa);
    message->ResponseVa = ViosndAllocControlBuffer(Device, ResponseLength, &message->ResponsePa);
    if (message->RequestVa == NULL || message->ResponseVa == NULL) {
        ViosndFreeControlMessage(Device, message);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    message->RequestLength = RequestLength;
    message->ResponseLength = ResponseLength;
    RtlCopyMemory(message->RequestVa, Request, RequestLength);
    KeMemoryBarrier();

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

    ViosndPublishDmaWrites();
    virtqueue_kick_always(Device->Queues[VIRTIO_SND_VQ_CONTROL]);

    do {
        used = virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_CONTROL], &usedLength);
        if (used != NULL) {
            PVIOSND_CONTROL_MESSAGE completed = (PVIOSND_CONTROL_MESSAGE)used;

            if (completed != message) {
                ViosndFreeControlMessage(Device, completed);
                continue;
            }

            for (ULONG i = 0; i < 100; ++i) {
                KeMemoryBarrier();
                if (ResponseLength < sizeof(VIRTIO_SND_HDR) ||
                    ((VIRTIO_SND_HDR *)message->ResponseVa)->code != 0) {
                    break;
                }
                KeStallExecutionProcessor(10);
            }
            RtlCopyMemory(Response, message->ResponseVa, ResponseLength);
            if (((const VIRTIO_SND_HDR *)Request)->code >= VIRTIO_SND_R_PCM_INFO) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: control complete code=0x%x used=%u response0=0x%x\n",
                           ((const VIRTIO_SND_HDR *)Request)->code,
                           usedLength,
                           ((const VIRTIO_SND_HDR *)Response)->code);
            }
            status = STATUS_SUCCESS;
            ViosndFreeControlMessage(Device, message);
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
        if ((elapsedUs % VIOSND_CONTROL_RENOTIFY_US) == 0) {
            virtqueue_kick_always(Device->Queues[VIRTIO_SND_VQ_CONTROL]);
        }
    } while (elapsedUs < VIOSND_CONTROL_TIMEOUT_MS * 1000u);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: control command timeout code=0x%x request=%u response=%u elapsedUs=%u\n",
               ((const VIRTIO_SND_HDR *)Request)->code,
               RequestLength,
               ResponseLength,
               elapsedUs);
    status = STATUS_IO_TIMEOUT;

Exit:
    if (lockHeld) {
        KeReleaseMutex(&Device->ControlQueueMutex, FALSE);
    }
    if (message != NULL && (!queued || status != STATUS_IO_TIMEOUT)) {
        ViosndFreeControlMessage(Device, message);
    }
    return status;
}

static NTSTATUS
ViosndControlStatusCommandEx(
    _Inout_ PVIOSND_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Out_opt_ PULONG ResponseCode)
{
    VIRTIO_SND_HDR response;
    ULONG command;
    NTSTATUS status;

    command = ((const VIRTIO_SND_HDR *)Request)->code;
    if (ResponseCode != NULL) {
        *ResponseCode = 0;
    }

    for (ULONG attempt = 0; attempt < VIOSND_CONTROL_STATUS_RETRY_COUNT; ++attempt) {
        RtlZeroMemory(&response, sizeof(response));
        status = ViosndControlCommand(Device, Request, RequestLength, &response, sizeof(response));
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (ResponseCode != NULL) {
            *ResponseCode = response.code;
        }

        if (!ViosndIsPcmStatusCommand(command) ||
            response.code == VIRTIO_SND_S_OK) {
            break;
        }

        if (response.code == 0 && attempt + 1 < VIOSND_CONTROL_STATUS_RETRY_COUNT) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_WARNING_LEVEL,
                       "viosnd: zero pcm status command=0x%x attempt=%u retrying\n",
                       command,
                       attempt + 1);
            ViosndDelayMilliseconds(VIOSND_CONTROL_STATUS_RETRY_DELAY_MS);
            continue;
        }

        if (response.code == VIRTIO_SND_S_BAD_MSG &&
            attempt + 1 < VIOSND_PCM_BAD_MSG_RETRY_COUNT) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_WARNING_LEVEL,
                       "viosnd: BAD_MSG pcm status command=0x%x attempt=%u request=%u retrying\n",
                       command,
                       attempt + 1,
                       RequestLength);
            ViosndDelayMilliseconds(VIOSND_PCM_BAD_MSG_RETRY_DELAY_MS);
            continue;
        }

        break;
    }

    if (response.code != VIRTIO_SND_S_OK) {
        if (response.code == 0 && ViosndIsPcmStatusCommand(command)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_WARNING_LEVEL,
                       "viosnd: accepting zero pcm status command=0x%x after retries\n",
                       command);
            return STATUS_SUCCESS;
        }

        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: control status command=0x%x response=0x%x pcm=%u\n",
                   command,
                   response.code,
                   ViosndIsPcmStatusCommand(command));
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndControlStatusCommand(
    _Inout_ PVIOSND_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength)
{
    return ViosndControlStatusCommandEx(Device, Request, RequestLength, NULL);
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
            KeMemoryBarrier();
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
    KeInitializeMutex(&device->TxQueueMutex, 0);
    KeInitializeMutex(&device->RxQueueMutex, 0);
    KeInitializeMutex(&device->EventQueueMutex, 0);
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

    status = ViosndInitializeDmaAdapter(device);
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

    for (ULONG i = 0; i < VIOSND_EVENT_BUFFER_COUNT; ++i) {
        ViosndFreeControlBuffer(Device, Device->EventBuffers[i].EventVa);
        Device->EventBuffers[i].EventVa = NULL;
    }

    for (ULONG i = 0; i < VIOSND_MAX_BARS; ++i) {
        if (Device->Bars[i].BaseVA != NULL) {
            MmUnmapIoSpace(Device->Bars[i].BaseVA, Device->Bars[i].Length);
        }
    }

    while (Device->DmaBlocks.Next != NULL) {
        PSINGLE_LIST_ENTRY entry = PopEntryList(&Device->DmaBlocks);
        PVIOSND_DMA_BLOCK block = CONTAINING_RECORD(entry, VIOSND_DMA_BLOCK, Entry);
        if (Device->DmaAdapter != NULL) {
            Device->DmaAdapter->DmaOperations->FreeCommonBuffer(Device->DmaAdapter,
                                                                (ULONG)block->Size,
                                                                block->LogicalAddress,
                                                                block->Va,
                                                                FALSE);
        }
        ExFreePoolWithTag(block, VIOSND_POOL_TAG);
    }

    if (Device->DmaAdapter != NULL) {
        Device->DmaAdapter->DmaOperations->PutDmaAdapter(Device->DmaAdapter);
        Device->DmaAdapter = NULL;
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

static BOOLEAN
ViosndPcmInfoLooksEmpty(
    _In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
    _In_ ULONG InfoCount)
{
    for (ULONG i = 0; i < InfoCount; ++i) {
        if (Info[i].direction != 0 ||
            Info[i].channels_min != 0 ||
            Info[i].channels_max != 0 ||
            Info[i].formats != 0 ||
            Info[i].rates != 0 ||
            Info[i].features != 0) {
            return FALSE;
        }
    }
    return TRUE;
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

    for (ULONG attempt = 0; attempt < VIOSND_PCM_INFO_RETRY_COUNT; ++attempt) {
        RtlZeroMemory(response, responseLength);
        RtlZeroMemory(info, sizeof(info));
        RtlZeroMemory(&responseHeader, sizeof(responseHeader));

        status = ViosndControlCommand(Device,
                                      &query,
                                      sizeof(query),
                                      response,
                                      responseLength);
        if (!NT_SUCCESS(status)) {
            break;
        }

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

        if (responseHeader.code == 0 ||
            (responseHeader.code == VIRTIO_SND_S_OK &&
             ViosndPcmInfoLooksEmpty(info, streamCount))) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_WARNING_LEVEL,
                       "viosnd: PCM_INFO invalid/empty response attempt=%u status=0x%08x retrying\n",
                       attempt + 1,
                       responseHeader.code);

            if (attempt + 1 < VIOSND_PCM_INFO_RETRY_COUNT) {
                LARGE_INTEGER interval;

                interval.QuadPart = -(10LL * 1000LL * VIOSND_PCM_INFO_RETRY_DELAY_MS);
                KeDelayExecutionThread(KernelMode, FALSE, &interval);
                continue;
            }
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
        break;
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PCM_INFO command failed 0x%08x\n",
                   status);
    }

    ExFreePoolWithTag(response, VIOSND_POOL_TAG);
    return status;
}

static NTSTATUS
ViosndConfigurePcmFormat(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ const VIOSND_PCM_FORMAT *Format)
{
    VIRTIO_SND_PCM_SET_PARAMS params;
    VIRTIO_SND_PCM_HDR command;
    NTSTATUS status;
    ULONG prepareResponse = 0;

    ViosndBuildSetParams(&params, StreamId, Format);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: configure stream=%u buffer=%u period=%u channels=%u format=%u rate=%u\n",
               StreamId,
               params.buffer_bytes,
               params.period_bytes,
               params.channels,
               params.format,
               params.rate);
    status = ViosndControlStatusCommand(Device, &params, sizeof(params));
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: SET_PARAMS failed stream=%u status=0x%08x\n",
                   StreamId,
                   status);
        return status;
    }

    RtlZeroMemory(&command, sizeof(command));
    command.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    command.stream_id = StreamId;
    status = ViosndControlStatusCommandEx(Device, &command, sizeof(command), &prepareResponse);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PREPARE failed stream=%u status=0x%08x response=0x%x\n",
                   StreamId,
                   status,
                   prepareResponse);
    }
    return status;
}

NTSTATUS
ViosndConfigureDefaultPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    VIOSND_PCM_FORMAT format;

    ViosndGetDefaultPcmFormat(&format);
    return ViosndConfigurePcmFormat(Device, StreamId, &format);
}

NTSTATUS
ViosndConfigureFallbackPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId)
{
    VIOSND_PCM_FORMAT format;

    ViosndGetFallbackPcmFormat(&format);
    return ViosndConfigurePcmFormat(Device, StreamId, &format);
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
    request = (struct _WRITE_MESSAGE *)ViosndAllocControlBuffer(Device,
                                                                requestLength,
                                                                &requestPa);
    status = (VIRTIO_SND_PCM_STATUS *)ViosndAllocControlBuffer(Device,
                                                               sizeof(*status),
                                                               &statusPa);
    if (request == NULL || status == NULL) {
        ViosndFreeControlBuffer(Device, request);
        ViosndFreeControlBuffer(Device, status);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    request->Header.stream_id = StreamId;
    RtlCopyMemory((PUCHAR)request + sizeof(*request), Buffer, Length);
    KeMemoryBarrier();

    sg[0].physAddr = requestPa;
    sg[0].length = requestLength;
    sg[1].physAddr = statusPa;
    sg[1].length = sizeof(*status);

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_TX], sg, 1, 1, request, NULL, 0) < 0) {
        result = STATUS_DEVICE_BUSY;
    } else {
        ViosndKickPcmQueue(Device->Queues[VIRTIO_SND_VQ_TX]);
        result = ViosndWaitForUsedBuffer(Device->Queues[VIRTIO_SND_VQ_TX], request, NULL);
        if (NT_SUCCESS(result)) {
            result = status->status == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
            if (NT_SUCCESS(result) && BytesWritten != NULL) {
                *BytesWritten = Length;
            }
        }
    }

    ViosndFreeControlBuffer(Device, request);
    ViosndFreeControlBuffer(Device, status);
    return result;
}

NTSTATUS
ViosndAllocateWritePcmIo(
    _Inout_ PVIOSND_DEVICE Device,
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
    io->RequestVa = ViosndAllocControlBuffer(Device, io->RequestLength, &io->RequestPa);
    io->StatusVa = (VIRTIO_SND_PCM_STATUS *)ViosndAllocControlBuffer(Device,
                                                                     sizeof(*io->StatusVa),
                                                                     &io->StatusPa);
    if (io->RequestVa == NULL || io->StatusVa == NULL) {
        ViosndFreeControlBuffer(Device, io->RequestVa);
        ViosndFreeControlBuffer(Device, io->StatusVa);
        ExFreePoolWithTag(io, VIOSND_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Io = io;
    return STATUS_SUCCESS;
}

VOID
ViosndFreeWritePcmIo(
    _Inout_ PVIOSND_DEVICE Device,
    _In_opt_ PVIOSND_PCM_IO Io)
{
    if (Io == NULL) {
        return;
    }

    ViosndFreeControlBuffer(Device, Io->RequestVa);
    ViosndFreeControlBuffer(Device, Io->StatusVa);
    ExFreePoolWithTag(Io, VIOSND_POOL_TAG);
}

NTSTATUS
ViosndAllocateReadPcmIo(
    _Inout_ PVIOSND_DEVICE Device,
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

    io->RequestLength = sizeof(VIRTIO_SND_PCM_XFER);
    io->RequestVa = ViosndAllocControlBuffer(Device, io->RequestLength, &io->RequestPa);
    io->AudioVa = ViosndAllocControlBuffer(Device,
                                           MaxAudioLength + sizeof(VIRTIO_SND_PCM_STATUS),
                                           &io->AudioPa);
    if (io->RequestVa == NULL || io->AudioVa == NULL) {
        ViosndFreeControlBuffer(Device, io->RequestVa);
        ViosndFreeControlBuffer(Device, io->AudioVa);
        ExFreePoolWithTag(io, VIOSND_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    io->MaxAudioLength = MaxAudioLength;
    *Io = io;
    return STATUS_SUCCESS;
}

VOID
ViosndFreeReadPcmIo(
    _Inout_ PVIOSND_DEVICE Device,
    _In_opt_ PVIOSND_PCM_IO Io)
{
    if (Io == NULL) {
        return;
    }

    ViosndFreeControlBuffer(Device, Io->RequestVa);
    ViosndFreeControlBuffer(Device, Io->AudioVa);
    ExFreePoolWithTag(Io, VIOSND_POOL_TAG);
}

NTSTATUS
ViosndSubmitPreparedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _In_ ULONG SourceLength,
    _In_ ULONG PacketNumber,
    _Inout_ PVIOSND_PCM_IO Io)
{
    VIRTIO_SND_PCM_XFER *header;
    VirtIOBufferDescriptor sg[2];
    ULONG requestLength;
    NTSTATUS status;

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
    Io->SourceLength = SourceLength != 0 ? SourceLength : Length;
    Io->PacketNumber = PacketNumber;
    Io->StreamId = StreamId;
    KeMemoryBarrier();

    sg[0].physAddr = Io->RequestPa;
    sg[0].length = requestLength;
    sg[1].physAddr = Io->StatusPa;
    sg[1].length = sizeof(*Io->StatusVa);

    status = ViosndAcquireTxQueue(Device, "submit");
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_TX], sg, 1, 1, Io, NULL, 0) < 0) {
        KeReleaseMutex(&Device->TxQueueMutex, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    ViosndPublishDmaWrites();
    ViosndKickPcmQueue(Device->Queues[VIRTIO_SND_VQ_TX]);
    KeReleaseMutex(&Device->TxQueueMutex, FALSE);
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

    status = ViosndAllocateWritePcmIo(Device, Length, &io);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ViosndSubmitPreparedWritePcm(Device, StreamId, Buffer, Length, Length, 0, io);
    if (!NT_SUCCESS(status)) {
        ViosndFreeWritePcmIo(Device, io);
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
    ULONG pcmStatus;

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

    status = ViosndAcquireTxQueue(Device, "reclaim");
    if (!NT_SUCCESS(status)) {
        return status;
    }
    io = (PVIOSND_PCM_IO)virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_TX], &usedLength);
    KeReleaseMutex(&Device->TxQueueMutex, FALSE);
    if (io == NULL) {
        return STATUS_NOT_FOUND;
    }

    KeMemoryBarrier();
    /*
     * For playback the used buffer is the reliable completion signal.  Some
     * transports have been observed to complete TX buffers while leaving the
     * optional status payload as zero, especially on ARM64, and failing the
     * reclaim path prevents WaveRT position from advancing.
     */
    pcmStatus = io->StatusVa->status;
    status = STATUS_SUCCESS;
    if (pcmStatus != 0 && pcmStatus != VIRTIO_SND_S_OK) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: write complete stream=%u packet=%u used=%u pcmStatus=0x%x latency=%u length=%u accepted=1\n",
                   io->StreamId,
                   io->PacketNumber,
                   usedLength,
                   pcmStatus,
                   io->StatusVa->latency_bytes,
                   io->AudioLength);
    }
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
ViosndDetachUnusedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Outptr_opt_result_maybenull_ PVIOSND_PCM_IO *Io)
{
    PVIOSND_PCM_IO io;

    if (Io != NULL) {
        *Io = NULL;
    }

    if (Device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status = ViosndAcquireTxQueue(Device, "detach");
    if (!NT_SUCCESS(status)) {
        return status;
    }
    io = (PVIOSND_PCM_IO)virtqueue_detach_unused_buf(Device->Queues[VIRTIO_SND_VQ_TX]);
    KeReleaseMutex(&Device->TxQueueMutex, FALSE);
    if (io == NULL) {
        return STATUS_NOT_FOUND;
    }

    if (Io != NULL) {
        *Io = io;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndSubmitPreparedReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ ULONG Length,
    _In_ ULONG PacketNumber,
    _Inout_ PVIOSND_PCM_IO Io)
{
    VIRTIO_SND_PCM_XFER *header;
    VirtIOBufferDescriptor sg[2];

    if (Device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (Io == NULL ||
        Io->RequestVa == NULL ||
        Io->AudioVa == NULL ||
        Io->MaxAudioLength < Length) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Io->AudioVa, Length + sizeof(VIRTIO_SND_PCM_STATUS));
    header = (VIRTIO_SND_PCM_XFER *)Io->RequestVa;
    header->stream_id = StreamId;
    Io->StreamId = StreamId;
    Io->AudioLength = Length;
    Io->PacketNumber = PacketNumber;
    KeMemoryBarrier();

    sg[0].physAddr = Io->RequestPa;
    sg[0].length = sizeof(*header);
    sg[1].physAddr = Io->AudioPa;
    sg[1].length = Length + sizeof(VIRTIO_SND_PCM_STATUS);

    KeWaitForSingleObject(&Device->RxQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_RX], sg, 1, 1, Io, NULL, 0) < 0) {
        KeReleaseMutex(&Device->RxQueueMutex, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    ViosndPublishDmaWrites();
    ViosndKickPcmQueue(Device->Queues[VIRTIO_SND_VQ_RX]);
    KeReleaseMutex(&Device->RxQueueMutex, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndReclaimPreparedReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Outptr_opt_result_maybenull_ PVIOSND_PCM_IO *Io,
    _Out_opt_ PULONG BytesRead,
    _Out_opt_ PULONG LatencyBytes)
{
    unsigned int usedLength;
    PVIOSND_PCM_IO io;
    VIRTIO_SND_PCM_STATUS *statusVa;
    NTSTATUS status;

    if (Io != NULL) {
        *Io = NULL;
    }
    if (BytesRead != NULL) {
        *BytesRead = 0;
    }
    if (LatencyBytes != NULL) {
        *LatencyBytes = 0;
    }

    if (Device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    KeWaitForSingleObject(&Device->RxQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
    io = (PVIOSND_PCM_IO)virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_RX], &usedLength);
    KeReleaseMutex(&Device->RxQueueMutex, FALSE);
    if (io == NULL) {
        return STATUS_NOT_FOUND;
    }

    KeMemoryBarrier();
    if (usedLength < sizeof(VIRTIO_SND_PCM_STATUS)) {
        statusVa = NULL;
        status = STATUS_IO_DEVICE_ERROR;
    } else {
        statusVa = (VIRTIO_SND_PCM_STATUS *)((PUCHAR)io->AudioVa +
                                             usedLength -
                                             sizeof(VIRTIO_SND_PCM_STATUS));
        status = statusVa->status == VIRTIO_SND_S_OK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    }
    if (LatencyBytes != NULL && statusVa != NULL) {
        *LatencyBytes = statusVa->latency_bytes;
    }
    if (NT_SUCCESS(status) && BytesRead != NULL) {
        *BytesRead = min(io->AudioLength,
                         (ULONG)(usedLength - sizeof(VIRTIO_SND_PCM_STATUS)));
    }
    if (Io != NULL) {
        *Io = io;
    }
    return status;
}

ULONG
ViosndDetachUnusedReadPcm(
    _Inout_ PVIOSND_DEVICE Device)
{
    ULONG count = 0;

    if (Device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return 0;
    }

    KeWaitForSingleObject(&Device->RxQueueMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
    for (;;) {
        PVIOSND_PCM_IO io;

        io = (PVIOSND_PCM_IO)virtqueue_detach_unused_buf(Device->Queues[VIRTIO_SND_VQ_RX]);
        if (io == NULL) {
            break;
        }
        ViosndFreeReadPcmIo(Device, io);
        count++;
    }
    KeReleaseMutex(&Device->RxQueueMutex, FALSE);

    return count;
}

PVOID
ViosndGetPcmIoAudioBuffer(
    _In_ PVIOSND_PCM_IO Io)
{
    if (Io == NULL) {
        return NULL;
    }
    return Io->AudioVa != NULL ? Io->AudioVa :
                                 (PUCHAR)Io->RequestVa + sizeof(VIRTIO_SND_PCM_XFER);
}

ULONG
ViosndGetPcmIoPacketNumber(
    _In_ PVIOSND_PCM_IO Io)
{
    return Io != NULL ? Io->PacketNumber : 0;
}

ULONG
ViosndGetPcmIoSourceLength(
    _In_ PVIOSND_PCM_IO Io)
{
    return Io != NULL ? Io->SourceLength : 0;
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
        ViosndFreeWritePcmIo(Device, io);
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
    request = (VIRTIO_SND_PCM_XFER *)ViosndAllocControlBuffer(Device,
                                                              sizeof(*request),
                                                              &requestPa);
    response = ViosndAllocControlBuffer(Device, responseLength, &responsePa);
    if (request == NULL || response == NULL) {
        ViosndFreeControlBuffer(Device, request);
        ViosndFreeControlBuffer(Device, response);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    request->stream_id = StreamId;
    KeMemoryBarrier();
    sg[0].physAddr = requestPa;
    sg[0].length = sizeof(*request);
    sg[1].physAddr = responsePa;
    sg[1].length = responseLength;

    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_RX], sg, 1, 1, request, NULL, 0) < 0) {
        result = STATUS_DEVICE_BUSY;
    } else {
        ViosndKickPcmQueue(Device->Queues[VIRTIO_SND_VQ_RX]);
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

    ViosndFreeControlBuffer(Device, request);
    ViosndFreeControlBuffer(Device, response);
    return result;
}
