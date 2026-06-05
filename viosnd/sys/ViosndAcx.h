#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <windef.h>
#include <ks.h>
#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#include <ksmedia.h>
#include <ntstrsafe.h>

extern "C" {
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
#include "VirtIOWdf.h"
}

typedef size_t* WDF_STRUCT_INFO;

#include "acx/km/1.1/acx.h"

#include "trace.h"
#include "VirtioSnd.h"
#include "ViosndPcm.h"

#define VIOSND_POOL_TAG 'dnSV'
#define VIOSND_EVENT_BUFFER_COUNT 8u
#define VIOSND_MIN_PERIOD_COUNT 2u
#define VIOSND_MAX_PERIOD_COUNT 64u

extern BOOLEAN g_ViosndLogEnabled;

VOID
ViosndLogAlways(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

#define VIOSND_LOG(_component, _level, _format, ...) \
    ViosndLogAlways(_format, __VA_ARGS__)

#include <pshpack1.h>

typedef struct _VIOSND_WAVEFORMATEX {
    USHORT wFormatTag;
    USHORT nChannels;
    ULONG nSamplesPerSec;
    ULONG nAvgBytesPerSec;
    USHORT nBlockAlign;
    USHORT wBitsPerSample;
    USHORT cbSize;
} VIOSND_WAVEFORMATEX, *PVIOSND_WAVEFORMATEX;

typedef struct _VIOSND_WAVEFORMATEXTENSIBLE {
    VIOSND_WAVEFORMATEX Format;
    union {
        USHORT wValidBitsPerSample;
        USHORT wSamplesPerBlock;
        USHORT wReserved;
    } Samples;
    ULONG dwChannelMask;
    GUID SubFormat;
} VIOSND_WAVEFORMATEXTENSIBLE, *PVIOSND_WAVEFORMATEXTENSIBLE;

typedef struct _VIOSND_KSDATAFORMAT_WAVEFORMATEXTENSIBLE {
    KSDATAFORMAT DataFormat;
    VIOSND_WAVEFORMATEXTENSIBLE WaveFormatExt;
} VIOSND_KSDATAFORMAT_WAVEFORMATEXTENSIBLE, *PVIOSND_KSDATAFORMAT_WAVEFORMATEXTENSIBLE;

#include <poppack.h>

C_ASSERT(sizeof(VIOSND_WAVEFORMATEX) == 18);
C_ASSERT(sizeof(VIOSND_WAVEFORMATEXTENSIBLE) == 40);

typedef struct _VIOSND_EVENT_BUFFER {
    VIRTIO_SND_EVENT *EventVa;
    PHYSICAL_ADDRESS EventPa;
} VIOSND_EVENT_BUFFER, *PVIOSND_EVENT_BUFFER;

typedef struct _VIOSND_ACX_DEVICE {
    WDFDEVICE WdfDevice;
    VIRTIO_WDF_DRIVER Vio;
    WDFINTERRUPT QueueInterrupt;

    struct virtqueue *Queues[VIRTIO_SND_VQ_MAX];
    WDFWAITLOCK ControlLock;
    WDFSPINLOCK EventLock;
    WDFSPINLOCK TxLock;
    WDFSPINLOCK RxLock;

    VIRTIO_SND_CONFIG Config;
    VIOSND_STREAM_PAIR Streams;

    PVOID EventPoolVa;
    PHYSICAL_ADDRESS EventPoolPa;
    ULONG EventPoolBytes;
    VIOSND_EVENT_BUFFER EventBuffers[VIOSND_EVENT_BUFFER_COUNT];
    ULONG EventBufferCount;

    ACXCIRCUIT RenderCircuit;
    ACXCIRCUIT CaptureCircuit;
    BOOLEAN RenderCircuitAdded;
    BOOLEAN CaptureCircuitAdded;
    struct _VIOSND_ACX_STREAM *RenderStream;
    struct _VIOSND_ACX_STREAM *CaptureStream;

    BOOLEAN VirtioReady;
    BOOLEAN QueuesReady;
} VIOSND_ACX_DEVICE, *PVIOSND_ACX_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_DEVICE, ViosndGetDeviceContext)

typedef struct _VIOSND_ACX_PERIOD {
    ULONG Index;
    ULONGLONG PacketNumber;
    ULONG Length;
    PVOID RequestVa;
    PHYSICAL_ADDRESS RequestPa;
    ULONG RequestLength;
    PVOID AudioVa;
    PHYSICAL_ADDRESS AudioPa;
    VIRTIO_SND_PCM_STATUS *StatusVa;
    PHYSICAL_ADDRESS StatusPa;
    BOOLEAN InFlight;
    BOOLEAN Ready;
} VIOSND_ACX_PERIOD, *PVIOSND_ACX_PERIOD;

typedef struct _VIOSND_ACX_STREAM {
    PVIOSND_ACX_DEVICE Device;
    ACXSTREAM Stream;
    ULONG StreamId;
    BOOLEAN Capture;
    BOOLEAN Prepared;
    BOOLEAN Running;
    ULONG PacketCount;
    ULONG PacketSize;
    PACX_RTPACKET Packets;
    PVOID *PacketBuffers;
    PMDL *PacketMdls;
    ULONG PacketAllocBytes;
    ULONG FirstPacketOffset;
    VIOSND_ACX_PERIOD *Periods;
    ULONG PeriodCount;
    ULONG PeriodBytes;
    ULONG BufferBytes;
    ULONG *RenderReadyQueue;
    ULONG RenderReadyCapacity;
    ULONG RenderReadyHead;
    ULONG RenderReadyTail;
    ULONG RenderReadyCount;
    volatile LONG RenderPumpActive;
    ULONGLONG PacketsCompleted;
    ULONGLONG NextCapturePacket;
    ULONGLONG PositionBytes;
    ULONG LastLatencyBytes;
    ULONG XrunCount;
    ULONG CurrentPacketQueryCount;
    ULONG PresentationQueryCount;
} VIOSND_ACX_STREAM, *PVIOSND_ACX_STREAM;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_STREAM, ViosndGetStreamContext)

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ViosndEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP ViosndEvtDriverContextCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE ViosndEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ViosndEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY ViosndEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT ViosndEvtDeviceD0Exit;
EVT_WDF_INTERRUPT_ISR ViosndEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC ViosndEvtInterruptDpc;

VOID
ViosndAcxWriteDiag(
    _In_opt_ WDFDEVICE Device,
    _In_z_ PCWSTR Stage,
    _In_ NTSTATUS Status);

NTSTATUS
ViosndAcxCreateCircuit(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Capture,
    _In_ ULONG StreamId,
    _Out_ ACXCIRCUIT *Circuit);

VOID
ViosndAcxDestroyDevice(
    _Inout_ PVIOSND_ACX_DEVICE Device);

NTSTATUS
ViosndAcxInitializeVirtio(
    _Inout_ PVIOSND_ACX_DEVICE Device);

NTSTATUS
ViosndAcxQueryPcmStreams(
    _Inout_ PVIOSND_ACX_DEVICE Device);

PVOID
ViosndAcxAllocDma(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ ULONG Size,
    _Out_ PPHYSICAL_ADDRESS PhysicalAddress);

VOID
ViosndAcxFreeDma(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_opt_ PVOID Va);

VOID
ViosndAcxFreeEventBuffers(
    _Inout_ PVIOSND_ACX_DEVICE Device);

NTSTATUS
ViosndAcxControlStatusCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength);

NTSTATUS
ViosndAcxPcmCommand(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ ULONG Code);

NTSTATUS
ViosndAcxPostEventBufferLocked(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _Inout_ PVIOSND_EVENT_BUFFER Buffer);

VOID
ViosndAcxDrainQueues(
    _Inout_ PVIOSND_ACX_DEVICE Device);

VOID
ViosndAcxStopStreams(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ BOOLEAN Release);
