#pragma once

typedef struct _VIOSND_DEVICE VIOSND_DEVICE, *PVIOSND_DEVICE;
typedef struct _VIOSND_PCM_IO VIOSND_PCM_IO, *PVIOSND_PCM_IO;

NTSTATUS
ViosndCreateDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ PRESOURCELIST ResourceList,
    _Outptr_ PVIOSND_DEVICE *Device);

VOID
ViosndDestroyDevice(
    _In_opt_ PVIOSND_DEVICE Device);

NTSTATUS
ViosndInitializeDevice(
    _Inout_ PVIOSND_DEVICE Device);

VOID
ViosndPollEvents(
    _Inout_ PVIOSND_DEVICE Device);

NTSTATUS
ViosndQueryPcmStreams(
    _Inout_ PVIOSND_DEVICE Device,
    _Out_ PVIOSND_STREAM_PAIR Pair);

NTSTATUS
ViosndConfigureDefaultPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId);

NTSTATUS
ViosndStartPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId);

NTSTATUS
ViosndStopPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId);

NTSTATUS
ViosndReleasePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId);

NTSTATUS
ViosndWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG BytesWritten);

NTSTATUS
ViosndSubmitWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length);

NTSTATUS
ViosndAllocateWritePcmIo(
    _In_ ULONG MaxAudioLength,
    _Outptr_ PVIOSND_PCM_IO *Io);

VOID
ViosndFreeWritePcmIo(
    _In_opt_ PVIOSND_PCM_IO Io);

NTSTATUS
ViosndAllocateReadPcmIo(
    _In_ ULONG MaxAudioLength,
    _Outptr_ PVIOSND_PCM_IO *Io);

VOID
ViosndFreeReadPcmIo(
    _In_opt_ PVIOSND_PCM_IO Io);

NTSTATUS
ViosndSubmitPreparedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Inout_ PVIOSND_PCM_IO Io);

NTSTATUS
ViosndReclaimWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Out_opt_ PULONG BytesWritten);

NTSTATUS
ViosndReclaimPreparedWritePcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Outptr_opt_result_maybenull_ PVIOSND_PCM_IO *Io,
    _Out_opt_ PULONG BytesWritten,
    _Out_opt_ PULONG LatencyBytes);

NTSTATUS
ViosndSubmitPreparedReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ ULONG Length,
    _In_ ULONG PacketNumber,
    _Inout_ PVIOSND_PCM_IO Io);

NTSTATUS
ViosndReclaimPreparedReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _Outptr_opt_result_maybenull_ PVIOSND_PCM_IO *Io,
    _Out_opt_ PULONG BytesRead,
    _Out_opt_ PULONG LatencyBytes);

ULONG
ViosndDetachUnusedReadPcm(
    _Inout_ PVIOSND_DEVICE Device);

PVOID
ViosndGetPcmIoAudioBuffer(
    _In_ PVIOSND_PCM_IO Io);

ULONG
ViosndGetPcmIoPacketNumber(
    _In_ PVIOSND_PCM_IO Io);

NTSTATUS
ViosndReadPcm(
    _Inout_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _Out_writes_bytes_(Length) VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG BytesRead);
