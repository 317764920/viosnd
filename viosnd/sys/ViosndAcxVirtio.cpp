#include "ViosndAcx.h"

typedef struct _VIOSND_CONTROL_MESSAGE {
    PVOID RequestVa;
    PHYSICAL_ADDRESS RequestPa;
    ULONG RequestLength;
    PVOID ResponseVa;
    PHYSICAL_ADDRESS ResponsePa;
    ULONG ResponseLength;
} VIOSND_CONTROL_MESSAGE, *PVIOSND_CONTROL_MESSAGE;

static NTSTATUS
ViosndControlCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Out_writes_bytes_(ResponseLength) VOID *Response,
    _In_ ULONG ResponseLength);

PVOID
ViosndAcxAllocDma(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ ULONG Size,
    _Out_ PPHYSICAL_ADDRESS PhysicalAddress)
{
    PVOID va;

    va = VirtIOWdfDeviceAllocDmaMemory(&Device->Vio.VIODevice, Size, VIOSND_POOL_TAG);
    if (va == NULL) {
        return NULL;
    }

    RtlZeroMemory(va, Size);
    *PhysicalAddress = VirtIOWdfDeviceGetPhysicalAddress(&Device->Vio.VIODevice, va);
    return va;
}

VOID
ViosndAcxFreeDma(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_opt_ PVOID Va)
{
    if (Va != NULL) {
        VirtIOWdfDeviceFreeDmaMemory(&Device->Vio.VIODevice, Va);
    }
}

static VOID
ViosndFreeControlMessage(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_opt_ PVIOSND_CONTROL_MESSAGE Message)
{
    if (Message == NULL) {
        return;
    }

    ViosndAcxFreeDma(Device, Message->RequestVa);
    ViosndAcxFreeDma(Device, Message->ResponseVa);
    ExFreePoolWithTag(Message, VIOSND_POOL_TAG);
}

NTSTATUS
ViosndAcxPostEventBufferLocked(
    _Inout_ PVIOSND_ACX_DEVICE Device,
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
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG poolBytes;

    if (Device->Queues[VIRTIO_SND_VQ_EVENT] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    poolBytes = max(PAGE_SIZE, VIOSND_EVENT_BUFFER_COUNT * (ULONG)sizeof(VIRTIO_SND_EVENT));
    Device->EventPoolVa = ViosndAcxAllocDma(Device, poolBytes, &Device->EventPoolPa);
    if (Device->EventPoolVa == NULL) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: event pool allocation failed bytes=%u eventSize=%u count=%u\n",
                   poolBytes,
                   (ULONG)sizeof(VIRTIO_SND_EVENT),
                   VIOSND_EVENT_BUFFER_COUNT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Device->EventPoolBytes = poolBytes;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: event pool va=%p pa=0x%llx bytes=%u\n",
               Device->EventPoolVa,
               Device->EventPoolPa.QuadPart,
               Device->EventPoolBytes);

    WdfSpinLockAcquire(Device->EventLock);
    for (ULONG i = 0; i < VIOSND_EVENT_BUFFER_COUNT; ++i) {
        Device->EventBuffers[i].EventVa =
            (VIRTIO_SND_EVENT *)((PUCHAR)Device->EventPoolVa +
                                 i * sizeof(VIRTIO_SND_EVENT));
        Device->EventBuffers[i].EventPa.QuadPart =
            Device->EventPoolPa.QuadPart + i * sizeof(VIRTIO_SND_EVENT);

        status = ViosndAcxPostEventBufferLocked(Device, &Device->EventBuffers[i]);
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: event post failed index=%u status=0x%08x\n",
                       i,
                       status);
            break;
        }
        Device->EventBufferCount++;
    }

    if (NT_SUCCESS(status)) {
        virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_EVENT]);
    }
    WdfSpinLockRelease(Device->EventLock);

    return status;
}

VOID
ViosndAcxFreeEventBuffers(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    for (ULONG i = 0; i < VIOSND_EVENT_BUFFER_COUNT; ++i) {
        Device->EventBuffers[i].EventVa = NULL;
        Device->EventBuffers[i].EventPa.QuadPart = 0;
    }
    Device->EventBufferCount = 0;
    if (Device->EventPoolVa != NULL) {
        ViosndAcxFreeDma(Device, Device->EventPoolVa);
        Device->EventPoolVa = NULL;
        Device->EventPoolPa.QuadPart = 0;
        Device->EventPoolBytes = 0;
    }
}

NTSTATUS
ViosndAcxControlStatusCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength)
{
    VIRTIO_SND_HDR response;
    NTSTATUS status;

    RtlZeroMemory(&response, sizeof(response));
    status = ViosndControlCommand(Device,
                                  Request,
                                  RequestLength,
                                  &response,
                                  sizeof(response));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (response.code) {
    case VIRTIO_SND_S_OK:
        return STATUS_SUCCESS;
    case VIRTIO_SND_S_NOT_SUPP:
        return STATUS_NOT_SUPPORTED;
    case VIRTIO_SND_S_BAD_MSG:
        return STATUS_INVALID_PARAMETER;
    default:
        return STATUS_IO_DEVICE_ERROR;
    }
}

NTSTATUS
ViosndAcxPcmCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ ULONG Code)
{
    VIRTIO_SND_PCM_HDR command;

    RtlZeroMemory(&command, sizeof(command));
    command.hdr.code = Code;
    command.stream_id = StreamId;
    return ViosndAcxControlStatusCommand(Device, &command, sizeof(command));
}

static NTSTATUS
ViosndControlCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Out_writes_bytes_(ResponseLength) VOID *Response,
    _In_ ULONG ResponseLength)
{
    PVIOSND_CONTROL_MESSAGE message;
    VirtIOBufferDescriptor sg[2];
    ULONG elapsedUs = 0;
    unsigned int usedLength = 0;
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

    message->RequestVa = ViosndAcxAllocDma(Device, RequestLength, &message->RequestPa);
    message->ResponseVa = ViosndAcxAllocDma(Device, ResponseLength, &message->ResponsePa);
    if (message->RequestVa == NULL || message->ResponseVa == NULL) {
        ViosndFreeControlMessage(Device, message);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    message->RequestLength = RequestLength;
    message->ResponseLength = ResponseLength;
    RtlCopyMemory(message->RequestVa, Request, RequestLength);

    sg[0].physAddr = message->RequestPa;
    sg[0].length = RequestLength;
    sg[1].physAddr = message->ResponsePa;
    sg[1].length = ResponseLength;

    WdfWaitLockAcquire(Device->ControlLock, NULL);
    if (virtqueue_add_buf(Device->Queues[VIRTIO_SND_VQ_CONTROL],
                          sg,
                          1,
                          1,
                          message,
                          NULL,
                          0) < 0) {
        WdfWaitLockRelease(Device->ControlLock);
        ViosndFreeControlMessage(Device, message);
        return STATUS_DEVICE_BUSY;
    }
    virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_CONTROL]);

    for (;;) {
        PVOID used = virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_CONTROL],
                                       &usedLength);
        if (used == message) {
            RtlCopyMemory(Response, message->ResponseVa, ResponseLength);
            break;
        }

        if (elapsedUs >= 1000u * 1000u) {
            status = STATUS_IO_TIMEOUT;
            break;
        }

        LARGE_INTEGER interval;
        interval.QuadPart = -(10LL * 1000LL);
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        elapsedUs += 1000u;
    }

    WdfWaitLockRelease(Device->ControlLock);

    if (NT_SUCCESS(status)) {
        UNREFERENCED_PARAMETER(usedLength);
    }
    ViosndFreeControlMessage(Device, message);
    return status;
}

static NTSTATUS
ViosndQueryPcmInfo(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ ULONG Count,
    _Out_writes_(Count) VIRTIO_SND_PCM_INFO *Info)
{
    VIRTIO_SND_QUERY_INFO request;
    struct _PCM_INFO_RESPONSE {
        VIRTIO_SND_HDR Header;
        VIRTIO_SND_PCM_INFO Info[1];
    } *response;
    ULONG responseLength;
    NTSTATUS status;

    if (Count == 0 || Count > 64) {
        return STATUS_INVALID_PARAMETER;
    }

    responseLength = sizeof(VIRTIO_SND_HDR) + Count * sizeof(VIRTIO_SND_PCM_INFO);
    response = (struct _PCM_INFO_RESPONSE *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                        responseLength,
                                                                        VIOSND_POOL_TAG);
    if (response == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&request, sizeof(request));
    request.hdr.code = VIRTIO_SND_R_PCM_INFO;
    request.start_id = 0;
    request.count = Count;
    request.size = sizeof(VIRTIO_SND_PCM_INFO);

    status = ViosndControlCommand(Device,
                                  &request,
                                  sizeof(request),
                                  response,
                                  responseLength);
    if (NT_SUCCESS(status)) {
        if (response->Header.code != VIRTIO_SND_S_OK) {
            status = STATUS_IO_DEVICE_ERROR;
        } else {
            RtlCopyMemory(Info, response->Info, Count * sizeof(VIRTIO_SND_PCM_INFO));
        }
    }

    ExFreePoolWithTag(response, VIOSND_POOL_TAG);
    return status;
}

NTSTATUS
ViosndAcxInitializeVirtio(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    NTSTATUS status;
    ULONGLONG hostFeatures;
    ULONGLONG driverFeatures = 0;
    VIRTIO_WDF_QUEUE_PARAM queueParams[VIRTIO_SND_VQ_MAX];
    unsigned short queueEntries = 0;
    unsigned long queueRingSize = 0;
    unsigned long queueHeapSize = 0;

    ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.Start", STATUS_SUCCESS);

    hostFeatures = VirtIOWdfGetDeviceFeatures(&Device->Vio);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: host features 0x%016llx\n",
               hostFeatures);
    if (virtio_is_feature_enabled(hostFeatures, VIRTIO_F_VERSION_1)) {
        virtio_feature_enable(driverFeatures, VIRTIO_F_VERSION_1);
    }

    status = VirtIOWdfSetDriverFeatures(&Device->Vio, driverFeatures, 0);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.SetFeatures", status);
        return status;
    }

    RtlZeroMemory(queueParams, sizeof(queueParams));
    for (ULONG i = 0; i < VIRTIO_SND_VQ_MAX; ++i) {
        queueParams[i].Interrupt = Device->QueueInterrupt;
        status = virtio_query_queue_allocation(&Device->Vio.VIODevice,
                                               i,
                                               &queueEntries,
                                               &queueRingSize,
                                               &queueHeapSize);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: queue[%u] alloc status=0x%08x entries=%u ring=%lu heap=%lu\n",
                   i,
                   status,
                   queueEntries,
                   queueRingSize,
                   queueHeapSize);
    }

    ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.BeforeInitQueues", STATUS_SUCCESS);
    status = VirtIOWdfInitQueues(&Device->Vio,
                                 VIRTIO_SND_VQ_MAX,
                                 Device->Queues,
                                 queueParams);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.InitQueues", status);
        return status;
    }
    Device->QueuesReady = TRUE;
    ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.InitQueuesDone", STATUS_SUCCESS);

    VirtIOWdfDeviceGet(&Device->Vio,
                       0,
                       &Device->Config,
                       sizeof(Device->Config));
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: config jacks=%u streams=%u chmaps=%u controls=%u\n",
               Device->Config.jacks,
               Device->Config.streams,
               Device->Config.chmaps,
               Device->Config.controls);

    status = ViosndInitializeEventQueue(Device);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.EventQueue", status);
        return status;
    }
    ViosndAcxWriteDiag(Device->WdfDevice, L"InitializeVirtio.Done", STATUS_SUCCESS);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: virtio initialized streams=%u jacks=%u chmaps=%u controls=%u\n",
               Device->Config.streams,
               Device->Config.jacks,
               Device->Config.chmaps,
               Device->Config.controls);

    return STATUS_SUCCESS;
}

NTSTATUS
ViosndAcxQueryPcmStreams(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    VIRTIO_SND_PCM_INFO *info;
    ULONG streamCount;
    NTSTATUS status;

    streamCount = Device->Config.streams;
    if (streamCount == 0 || streamCount > 64) {
        return STATUS_NOT_FOUND;
    }

    info = (VIRTIO_SND_PCM_INFO *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                             streamCount * sizeof(*info),
                                                             VIOSND_POOL_TAG);
    if (info == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ViosndQueryPcmInfo(Device, streamCount, info);
    if (NT_SUCCESS(status)) {
        for (ULONG i = 0; i < streamCount; ++i) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: pcm[%u] dir=%u ch=%u-%u formats=0x%016llx rates=0x%016llx features=0x%08x\n",
                       i,
                       info[i].direction,
                       info[i].channels_min,
                       info[i].channels_max,
                       info[i].formats,
                       info[i].rates,
                       info[i].features);
        }
    }
    if (NT_SUCCESS(status)) {
        status = ViosndFindStreamPair(info, streamCount, &Device->Streams);
    }

    if (NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: selected render=%u/%u capture=%u/%u\n",
                   Device->Streams.HasRender,
                   Device->Streams.RenderStreamId,
                   Device->Streams.HasCapture,
                   Device->Streams.CaptureStreamId);
    }

    ExFreePoolWithTag(info, VIOSND_POOL_TAG);
    return status;
}

VOID
ViosndAcxDestroyDevice(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    if (Device->VirtioReady) {
        ViosndAcxStopStreams(Device, TRUE);
        Device->VirtioReady = FALSE;
    }

    if (Device->QueuesReady) {
        VirtIOWdfDestroyQueues(&Device->Vio);
        RtlZeroMemory(Device->Queues, sizeof(Device->Queues));
        Device->QueuesReady = FALSE;
    }

    ViosndAcxFreeEventBuffers(Device);

    VirtIOWdfShutdown(&Device->Vio);
}
