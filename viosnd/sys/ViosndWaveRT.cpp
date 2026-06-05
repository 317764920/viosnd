#include "precomp.h"

enum {
    VIOSND_NODE_DAC_ADC = 0
};

#define VIOSND_RENDER_IO_POOL_SIZE 12u
#define VIOSND_RENDER_TARGET_OUTSTANDING_PACKETS 8u
#define VIOSND_RENDER_CYCLIC_OUTSTANDING_PACKETS 8u
#define VIOSND_RENDER_CYCLIC_PREROLL_PACKETS VIOSND_RENDER_CYCLIC_OUTSTANDING_PACKETS
#define VIOSND_CAPTURE_IO_POOL_SIZE 8u
#define VIOSND_CAPTURE_TARGET_OUTSTANDING_PACKETS 4u
#define VIOSND_COMPLETION_STALL_LOG_LOOPS 200u
#define VIOSND_RENDER_POLL_INTERVAL_US 1000LL
#define VIOSND_CAPTURE_POLL_INTERVAL_US 1000LL
#define VIOSND_CAPTURE_STOP_RECLAIM_POLLS 2u
#define VIOSND_CAPTURE_STOP_TIMEOUT_MS 50LL
#define VIOSND_CAPTURE_SOFTWARE_GAIN 1
#define VIOSND_CAPTURE_NOISE_GATE_PEAK 0u
#define VIOSND_PERIODIC_LOG_MASK 0x1ffu
#define VIOSND_RENDER_SOFTWARE_CLOCK 1

static volatile LONG ViosndCaptureFaulted;
static UCHAR ViosndSilencePeriod[VIOSND_DEFAULT_PERIOD_BYTES];

static KSDATARANGE_AUDIO ViosndPinDataRangesStream[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        VIOSND_DEFAULT_CHANNELS,
        VIOSND_DEFAULT_BITS_PER_SAMPLE,
        VIOSND_DEFAULT_BITS_PER_SAMPLE,
        VIOSND_DEFAULT_SAMPLE_RATE,
        VIOSND_DEFAULT_SAMPLE_RATE
    }
};

static PKSDATARANGE ViosndPinDataRangePointersStream[] = {
    PKSDATARANGE(&ViosndPinDataRangesStream[0])
};

static KSDATARANGE ViosndPinDataRangesBridge[] = {
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE ViosndPinDataRangePointersBridge[] = {
    &ViosndPinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR ViosndRenderPins[] = {
    {
        1,
        1,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersStream),
            ViosndPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersBridge),
            ViosndPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

static PCNODE_DESCRIPTOR ViosndRenderNodes[] = {
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

static PCCONNECTION_DESCRIPTOR ViosndRenderConnections[] = {
    { PCFILTER_NODE, VIOSND_PIN_SYSTEM, VIOSND_NODE_DAC_ADC, 1 },
    { VIOSND_NODE_DAC_ADC, 0, PCFILTER_NODE, VIOSND_PIN_BRIDGE }
};

static PCFILTER_DESCRIPTOR ViosndRenderFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndRenderPins),
    ViosndRenderPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndRenderNodes),
    ViosndRenderNodes,
    SIZEOF_ARRAY(ViosndRenderConnections),
    ViosndRenderConnections,
    0,
    NULL
};

static PCPIN_DESCRIPTOR ViosndCapturePins[] = {
    {
        1,
        1,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersStream),
            ViosndPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersBridge),
            ViosndPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

static PCNODE_DESCRIPTOR ViosndCaptureNodes[] = {
    { 0, NULL, &KSNODETYPE_ADC, NULL }
};

static PCCONNECTION_DESCRIPTOR ViosndCaptureConnections[] = {
    { PCFILTER_NODE, VIOSND_PIN_BRIDGE, VIOSND_NODE_DAC_ADC, 1 },
    { VIOSND_NODE_DAC_ADC, 0, PCFILTER_NODE, VIOSND_PIN_SYSTEM }
};

static PCFILTER_DESCRIPTOR ViosndCaptureFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndCapturePins),
    ViosndCapturePins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndCaptureNodes),
    ViosndCaptureNodes,
    SIZEOF_ARRAY(ViosndCaptureConnections),
    ViosndCaptureConnections,
    0,
    NULL
};

static VOID
ViosndBuildDefaultFormat(
    _Out_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Format)
{
    RtlZeroMemory(Format, sizeof(*Format));
    Format->DataFormat.FormatSize = sizeof(*Format);
    Format->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Format->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    Format->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    Format->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Format->WaveFormatExt.Format.nChannels = VIOSND_DEFAULT_CHANNELS;
    Format->WaveFormatExt.Format.nSamplesPerSec = VIOSND_DEFAULT_SAMPLE_RATE;
    Format->WaveFormatExt.Format.wBitsPerSample = VIOSND_DEFAULT_BITS_PER_SAMPLE;
    Format->WaveFormatExt.Format.nBlockAlign =
        (VIOSND_DEFAULT_CHANNELS * VIOSND_DEFAULT_BITS_PER_SAMPLE) / 8;
    Format->WaveFormatExt.Format.nAvgBytesPerSec =
        Format->WaveFormatExt.Format.nSamplesPerSec * Format->WaveFormatExt.Format.nBlockAlign;
    Format->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    Format->WaveFormatExt.Samples.wValidBitsPerSample = VIOSND_DEFAULT_BITS_PER_SAMPLE;
    Format->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    Format->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

static BOOLEAN
ViosndIsDefaultFormat(
    _In_opt_ PKSDATAFORMAT DataFormat)
{
    PWAVEFORMATEX waveFormat;
    ULONG blockAlign = (VIOSND_DEFAULT_CHANNELS * VIOSND_DEFAULT_BITS_PER_SAMPLE) / 8;

    if (DataFormat == NULL ||
        !IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return FALSE;
    }

    waveFormat = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    if (waveFormat->nChannels != VIOSND_DEFAULT_CHANNELS ||
        waveFormat->nSamplesPerSec != VIOSND_DEFAULT_SAMPLE_RATE ||
        waveFormat->wBitsPerSample != VIOSND_DEFAULT_BITS_PER_SAMPLE ||
        waveFormat->nBlockAlign != blockAlign ||
        waveFormat->nAvgBytesPerSec != VIOSND_DEFAULT_SAMPLE_RATE * blockAlign) {
        return FALSE;
    }

    if (waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
        return IsEqualGUIDAligned(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)
                   ? TRUE
                   : FALSE;
    }

    if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE extensible =
            (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)DataFormat;

        return extensible->WaveFormatExt.Samples.wValidBitsPerSample ==
                       VIOSND_DEFAULT_BITS_PER_SAMPLE &&
                   IsEqualGUIDAligned(extensible->WaveFormatExt.SubFormat,
                                      KSDATAFORMAT_SUBTYPE_PCM)
                   ? TRUE
                   : FALSE;
    }

    return FALSE;
}

static ULONG
ViosndTargetOutstandingPackets(
    _In_ ULONG NotificationCount)
{
    if (NotificationCount == 0) {
        return 0;
    }

    if (NotificationCount == 1) {
        return 1;
    }

    return min(NotificationCount - 1, VIOSND_RENDER_TARGET_OUTSTANDING_PACKETS);
}

static ULONG
ViosndCyclicTargetOutstandingPackets(
    _In_ ULONG NotificationCount)
{
    if (NotificationCount == 0) {
        return 0;
    }

    if (NotificationCount == 1) {
        return 1;
    }

    return min(NotificationCount - 1, VIOSND_RENDER_CYCLIC_OUTSTANDING_PACKETS);
}

static BOOLEAN
ViosndPacketNumberLessOrEqual(
    _In_ ULONG Left,
    _In_ ULONG Right)
{
    return (LONG)(Left - Right) <= 0;
}

static BOOLEAN
ViosndHasWritableRenderPacket(
    _In_ ULONG NextSubmitPacket,
    _In_ ULONG LastOsWritePacket)
{
    return LastOsWritePacket != MAXULONG &&
           ViosndPacketNumberLessOrEqual(NextSubmitPacket, LastOsWritePacket);
}

static ULONG
ViosndPacketSizeFromNotificationCount(
    _In_ ULONG BufferSize,
    _In_ ULONG NotificationCount)
{
    UNREFERENCED_PARAMETER(NotificationCount);

    if (BufferSize < VIOSND_DEFAULT_PERIOD_BYTES) {
        return BufferSize;
    }

    return VIOSND_DEFAULT_PERIOD_BYTES;
}

static ULONG
ViosndNotificationCountFromBufferSize(
    _In_ ULONG BufferSize)
{
    if (BufferSize == 0) {
        return 0;
    }

    return max(BufferSize / VIOSND_DEFAULT_PERIOD_BYTES, 1u);
}

static ULONG
ViosndUsableBufferSizeFromAllocatedSize(
    _In_ ULONG AllocatedSize)
{
    if (AllocatedSize < VIOSND_DEFAULT_PERIOD_BYTES) {
        return AllocatedSize;
    }

    ULONG notificationCount = ViosndNotificationCountFromBufferSize(AllocatedSize);

    return notificationCount * VIOSND_DEFAULT_PERIOD_BYTES;
}

static ULONGLONG
ViosndRenderBytesFromQpc(
    _In_ LONGLONG QpcDelta,
    _In_ LONGLONG QpcFrequency)
{
    const ULONGLONG bytesPerSecond =
        (ULONGLONG)VIOSND_DEFAULT_SAMPLE_RATE *
        VIOSND_DEFAULT_CHANNELS *
        (VIOSND_DEFAULT_BITS_PER_SAMPLE / 8u);
    const ULONGLONG blockAlign =
        VIOSND_DEFAULT_CHANNELS * (VIOSND_DEFAULT_BITS_PER_SAMPLE / 8u);
    ULONGLONG bytes;

    if (QpcDelta <= 0 || QpcFrequency <= 0) {
        return 0;
    }

    bytes = ((ULONGLONG)QpcDelta * bytesPerSecond) / (ULONGLONG)QpcFrequency;
    return bytes - (bytes % blockAlign);
}

static ULONG
ViosndPcmPeak16(
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    const SHORT *samples = (const SHORT *)Buffer;
    ULONG sampleCount = Length / sizeof(SHORT);
    ULONG peak = 0;

    for (ULONG i = 0; i < sampleCount; ++i) {
        LONG sample = samples[i];
        ULONG magnitude = sample < 0 ? (ULONG)-sample : (ULONG)sample;

        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    return peak;
}

static ULONG
ViosndPcmCopyGain16(
    _Out_writes_bytes_(Length) VOID *Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length,
    _In_ LONG Gain)
{
    SHORT *destination = (SHORT *)Destination;
    const SHORT *source = (const SHORT *)Source;
    ULONG sampleCount = Length / sizeof(SHORT);
    ULONG peak = 0;

    for (ULONG i = 0; i < sampleCount; ++i) {
        LONG sample = (LONG)source[i] * Gain;

        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        destination[i] = (SHORT)sample;

        ULONG magnitude = sample < 0 ? (ULONG)-sample : (ULONG)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if ((Length & 1u) != 0) {
        ((PUCHAR)Destination)[Length - 1] = ((const UCHAR *)Source)[Length - 1];
    }

    return peak;
}

static VOID
ViosndRenderThread(_In_ PVOID Context);

class CViosndMiniportWaveRT;

class CViosndMiniportWaveRTStream :
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTInputStream,
    public IMiniportWaveRTOutputStream
{
    friend VOID ViosndRenderThread(_In_ PVOID Context);

public:
    CViosndMiniportWaveRTStream(
        _In_ CViosndMiniportWaveRT *Miniport,
        _In_ PVIOSND_DEVICE Device,
        _In_ ULONG StreamId,
        _In_ BOOLEAN Capture);

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTInputStream;
    IMP_IMiniportWaveRTOutputStream;

    STDMETHODIMP_(NTSTATUS) QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

private:
    LONG m_RefCount;
    CViosndMiniportWaveRT *m_Miniport;
    PVIOSND_DEVICE m_Device;
    ULONG m_StreamId;
    BOOLEAN m_Capture;
    KSSTATE m_State;
    PMDL m_Mdl;
    PVOID m_Buffer;
    ULONG m_BufferSize;
    ULONG m_NotificationCount;
    ULONG m_PacketSize;
    ULONG m_PacketNumber;
    ULONG m_LastOsWritePacket;
    ULONG m_NextSubmitPacket;
    ULONG m_NextReadPacket;
    ULONG m_EosPacketNumber;
    ULONG m_EosPacketLength;
    ULONG m_OutstandingWrites;
    ULONG m_RenderUnderruns;
    ULONG m_RenderFallbackPackets;
    ULONG m_RenderPrerollPackets;
    ULONG m_CaptureInFlight;
    ULONG m_CaptureOverflows;
    ULONG m_RenderStallLoops;
    ULONG m_CaptureStallLoops;
    ULONG m_CapturePositionQueries;
    ULONG m_CaptureReadPackets;
    PVIOSND_PCM_IO m_RenderIoPool[VIOSND_RENDER_IO_POOL_SIZE];
    ULONG m_RenderIoFreeCount;
    PVIOSND_PCM_IO m_CaptureIoPool[VIOSND_CAPTURE_IO_POOL_SIZE];
    ULONG m_CaptureIoFreeCount;
    ULONGLONG m_Position;
    ULONGLONG m_RunStartPosition;
    LONGLONG m_RunStartQpc;
    LONGLONG m_QpcFrequency;
    ULONG m_RenderNotReadyLoops;
    PKEVENT m_NotificationEvent;
    KEVENT m_StopEvent;
    KEVENT m_KickEvent;
    PVOID m_ThreadObject;
    BOOLEAN m_WorkerStarted;

    NTSTATUS SubmitRenderPacket(_In_ ULONG PacketNumber, _In_ ULONG PacketLength);
    NTSTATUS SubmitRenderSilencePacket(_In_ ULONG PacketNumber);
    VOID ReclaimRenderPackets(_Inout_ PULONG SubmittedSinceLog);
    NTSTATUS AllocateRenderIoPool();
    VOID FreeRenderIoPool();
    NTSTATUS AllocateCaptureIoPool();
    VOID FreeCaptureIoPool();
    NTSTATUS SubmitCapturePacket();
    VOID ReclaimCapturePackets();
    NTSTATUS StartRenderWorker();
    BOOLEAN StopRenderWorker();
    ULONGLONG GetRenderClockPosition();
    VOID RenderWorkerLoop();
    VOID CaptureWorkerLoop();
};

class CViosndMiniportWaveRT : public IMiniportWaveRT
{
public:
    CViosndMiniportWaveRT(_In_ PVIOSND_DEVICE Device, _In_ ULONG StreamId, _In_ BOOLEAN Capture);

    IMP_IMiniportWaveRT;

    STDMETHODIMP_(NTSTATUS) QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    BOOLEAN IsCapture() const { return m_Capture; }

private:
    LONG m_RefCount;
    PVIOSND_DEVICE m_Device;
    ULONG m_StreamId;
    BOOLEAN m_Capture;
    PPORTWAVERT m_Port;
};

CViosndMiniportWaveRTStream::CViosndMiniportWaveRTStream(
    _In_ CViosndMiniportWaveRT *Miniport,
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture) :
    m_RefCount(1),
    m_Miniport(Miniport),
    m_Device(Device),
    m_StreamId(StreamId),
    m_Capture(Capture),
    m_State(KSSTATE_STOP),
    m_Mdl(NULL),
    m_Buffer(NULL),
    m_BufferSize(0),
    m_NotificationCount(0),
    m_PacketSize(0),
    m_PacketNumber(0),
    m_LastOsWritePacket(MAXULONG),
    m_NextSubmitPacket(0),
    m_NextReadPacket(0),
    m_EosPacketNumber(MAXULONG),
    m_EosPacketLength(0),
    m_OutstandingWrites(0),
    m_RenderUnderruns(0),
    m_RenderFallbackPackets(0),
    m_RenderPrerollPackets(0),
    m_CaptureInFlight(0),
    m_CaptureOverflows(0),
    m_RenderStallLoops(0),
    m_CaptureStallLoops(0),
    m_CapturePositionQueries(0),
    m_CaptureReadPackets(0),
    m_RenderIoFreeCount(0),
    m_CaptureIoFreeCount(0),
    m_Position(0),
    m_RunStartPosition(0),
    m_RunStartQpc(0),
    m_QpcFrequency(0),
    m_RenderNotReadyLoops(0),
    m_NotificationEvent(NULL),
    m_ThreadObject(NULL),
    m_WorkerStarted(FALSE)
{
    KeInitializeEvent(&m_StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&m_KickEvent, SynchronizationEvent, FALSE);
    RtlZeroMemory(m_RenderIoPool, sizeof(m_RenderIoPool));
    RtlZeroMemory(m_CaptureIoPool, sizeof(m_CaptureIoPool));
    m_Miniport->AddRef();
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::QueryInterface(
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTStream) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTStreamNotification)) {
        *Interface = (IMiniportWaveRTStreamNotification *)this;
    } else if (m_Capture && IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTInputStream)) {
        *Interface = (IMiniportWaveRTInputStream *)this;
    } else if (!m_Capture && IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTOutputStream)) {
        *Interface = (IMiniportWaveRTOutputStream *)this;
    }

    if (*Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRTStream::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRTStream::Release()
{
    if (m_ThreadObject != NULL && PsGetCurrentThread() != m_ThreadObject) {
        StopRenderWorker();
    }

    LONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        if (m_ThreadObject != NULL && PsGetCurrentThread() == m_ThreadObject) {
            ObDereferenceObject(m_ThreadObject);
            m_ThreadObject = NULL;
            m_WorkerStarted = FALSE;
        } else {
            StopRenderWorker();
        }
        if (m_Mdl != NULL) {
            FreeAudioBuffer(m_Mdl, m_BufferSize);
        }
        FreeRenderIoPool();
        FreeCaptureIoPool();
        m_Miniport->Release();
        this->~CViosndMiniportWaveRTStream();
        ExFreePoolWithTag(this, VIOSND_POOL_TAG);
    }
    return (ULONG)count;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    return ViosndIsDefaultFormat(DataFormat) ? STATUS_SUCCESS : STATUS_NO_MATCH;
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitRenderPacket(
    _In_ ULONG PacketNumber,
    _In_ ULONG PacketLength)
{
    ULONG offset;
    ULONG length;
    PUCHAR packet;
    NTSTATUS status;
    PVIOSND_PCM_IO io;

    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (m_RenderIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }

    length = min(PacketLength, m_PacketSize);
    offset = (PacketNumber % max(m_NotificationCount, 1u)) * m_PacketSize;
    packet = (PUCHAR)m_Buffer + offset;
    if (PacketLength < m_PacketSize) {
        RtlZeroMemory(packet + length, m_PacketSize - length);
        length = m_PacketSize;
    }

    io = m_RenderIoPool[--m_RenderIoFreeCount];
    m_RenderIoPool[m_RenderIoFreeCount] = NULL;

    status = ViosndSubmitPreparedWritePcm(m_Device,
                                          m_StreamId,
                                          packet,
                                          length,
                                          io);
    if (NT_SUCCESS(status)) {
        m_NextSubmitPacket = PacketNumber + 1;
        m_OutstandingWrites++;
        if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 || PacketLength < m_PacketSize) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render submit packet=%u length=%u requested=%u outstanding=%u last=%u eos=%u/%u\n",
                       m_StreamId,
                       PacketNumber,
                       length,
                       PacketLength,
                       m_OutstandingWrites,
                       m_LastOsWritePacket,
                       m_EosPacketNumber,
                       m_EosPacketLength);
        }
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u submit render packet=%u length=%u failed 0x%08x\n",
                   m_StreamId,
                   PacketNumber,
                   length,
                   status);
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }

    return status;
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitRenderSilencePacket(
    _In_ ULONG PacketNumber)
{
    NTSTATUS status;
    PVIOSND_PCM_IO io;

    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (m_RenderIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }
    if (m_PacketSize > sizeof(ViosndSilencePeriod)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    io = m_RenderIoPool[--m_RenderIoFreeCount];
    m_RenderIoPool[m_RenderIoFreeCount] = NULL;

    status = ViosndSubmitPreparedWritePcm(m_Device,
                                          m_StreamId,
                                          ViosndSilencePeriod,
                                          m_PacketSize,
                                          io);
    if (NT_SUCCESS(status)) {
        m_NextSubmitPacket = PacketNumber + 1;
        m_OutstandingWrites++;
        if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render preroll silence packet=%u outstanding=%u remaining=%u\n",
                       m_StreamId,
                       PacketNumber,
                       m_OutstandingWrites,
                       m_RenderPrerollPackets);
        }
    } else {
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }
    return status;
}

VOID
CViosndMiniportWaveRTStream::ReclaimRenderPackets(_Inout_ PULONG SubmittedSinceLog)
{
    for (;;) {
        ULONG bytesWritten = 0;
        ULONG latencyBytes = 0;
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndReclaimPreparedWritePcm(m_Device,
                                                        &io,
                                                        &bytesWritten,
                                                        &latencyBytes);

        if (status == STATUS_NOT_FOUND) {
            return;
        }

        if (m_OutstandingWrites != 0) {
            m_OutstandingWrites--;
        }

        if (NT_SUCCESS(status)) {
            m_Position += bytesWritten;
            m_PacketNumber++;
            (*SubmittedSinceLog)++;
            if ((m_PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 ||
                latencyBytes > m_PacketSize * 4) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u render reclaim packet=%u bytes=%u pos=%llu latency=%u outstanding=%u next=%u last=%u\n",
                           m_StreamId,
                           m_PacketNumber,
                           bytesWritten,
                           m_Position,
                           latencyBytes,
                           m_OutstandingWrites,
                           m_NextSubmitPacket,
                           m_LastOsWritePacket);
            }
            if (m_NotificationEvent != NULL) {
                KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
            }
        } else {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u reclaim render packet failed 0x%08x outstanding=%u\n",
                       m_StreamId,
                       status,
                       m_OutstandingWrites);
        }

        if (io != NULL && m_RenderIoFreeCount < VIOSND_RENDER_IO_POOL_SIZE) {
            m_RenderIoPool[m_RenderIoFreeCount++] = io;
        } else {
            ViosndFreeWritePcmIo(io);
        }
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::AllocateRenderIoPool()
{
    if (m_Capture || m_RenderIoFreeCount != 0) {
        return STATUS_SUCCESS;
    }

    if (m_PacketSize == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    for (ULONG i = 0; i < VIOSND_RENDER_IO_POOL_SIZE; ++i) {
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndAllocateWritePcmIo(m_PacketSize, &io);

        if (!NT_SUCCESS(status)) {
            FreeRenderIoPool();
            return status;
        }
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u render preallocated tx buffers=%u packet=%u\n",
               m_StreamId,
               m_RenderIoFreeCount,
               m_PacketSize);
    return STATUS_SUCCESS;
}

VOID
CViosndMiniportWaveRTStream::FreeRenderIoPool()
{
    if (m_Capture) {
        return;
    }

    if (m_OutstandingWrites != 0) {
        return;
    }

    while (m_RenderIoFreeCount != 0) {
        PVIOSND_PCM_IO io = m_RenderIoPool[--m_RenderIoFreeCount];

        m_RenderIoPool[m_RenderIoFreeCount] = NULL;
        ViosndFreeWritePcmIo(io);
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::AllocateCaptureIoPool()
{
    if (!m_Capture || m_CaptureIoFreeCount != 0) {
        return STATUS_SUCCESS;
    }

    if (m_PacketSize == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    for (ULONG i = 0; i < VIOSND_CAPTURE_IO_POOL_SIZE; ++i) {
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndAllocateReadPcmIo(m_PacketSize, &io);

        if (!NT_SUCCESS(status)) {
            FreeCaptureIoPool();
            return status;
        }
        m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture preallocated rx buffers=%u packet=%u\n",
               m_StreamId,
               m_CaptureIoFreeCount,
               m_PacketSize);
    return STATUS_SUCCESS;
}

VOID
CViosndMiniportWaveRTStream::FreeCaptureIoPool()
{
    if (!m_Capture) {
        return;
    }

    if (m_CaptureInFlight != 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u capture io pool retained with in-flight=%u\n",
                   m_StreamId,
                   m_CaptureInFlight);
        return;
    }

    while (m_CaptureIoFreeCount != 0) {
        PVIOSND_PCM_IO io = m_CaptureIoPool[--m_CaptureIoFreeCount];

        m_CaptureIoPool[m_CaptureIoFreeCount] = NULL;
        ViosndFreeReadPcmIo(io);
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitCapturePacket()
{
    PVIOSND_PCM_IO io;
    NTSTATUS status;

    if (!m_Capture ||
        m_Buffer == NULL ||
        m_PacketSize == 0 ||
        m_NotificationCount == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_CaptureIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }

    if (m_CaptureInFlight >= m_NotificationCount) {
        return STATUS_DEVICE_BUSY;
    }

    io = m_CaptureIoPool[--m_CaptureIoFreeCount];
    m_CaptureIoPool[m_CaptureIoFreeCount] = NULL;

    status = ViosndSubmitPreparedReadPcm(m_Device,
                                         m_StreamId,
                                         m_PacketSize,
                                         m_NextSubmitPacket,
                                         io);
    if (NT_SUCCESS(status)) {
        m_NextSubmitPacket++;
        m_CaptureInFlight++;
    } else {
        m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
    }

    return status;
}

VOID
CViosndMiniportWaveRTStream::ReclaimCapturePackets()
{
    for (;;) {
        PVIOSND_PCM_IO io = NULL;
        ULONG bytesRead = 0;
        ULONG latencyBytes = 0;
        NTSTATUS status = ViosndReclaimPreparedReadPcm(m_Device,
                                                       &io,
                                                       &bytesRead,
                                                       &latencyBytes);

        if (status == STATUS_NOT_FOUND) {
            return;
        }

        if (m_CaptureInFlight != 0) {
            m_CaptureInFlight--;
        }

        if (NT_SUCCESS(status) && io != NULL) {
            ULONG packetNumber = ViosndGetPcmIoPacketNumber(io);
            ULONG offset = (packetNumber % m_NotificationCount) * m_PacketSize;
            PVOID source = ViosndGetPcmIoAudioBuffer(io);
            PUCHAR destination = (PUCHAR)m_Buffer + offset;
            ULONG copyLength = min(bytesRead, m_PacketSize);
            ULONG inputPeak = ViosndPcmPeak16(source, copyLength);
            ULONG outputPeak;

#if VIOSND_CAPTURE_NOISE_GATE_PEAK != 0
            if (inputPeak < VIOSND_CAPTURE_NOISE_GATE_PEAK) {
                RtlZeroMemory(destination, copyLength);
                outputPeak = 0;
            } else {
                outputPeak = ViosndPcmCopyGain16(destination,
                                                 source,
                                                 copyLength,
                                                 VIOSND_CAPTURE_SOFTWARE_GAIN);
            }
#else
            outputPeak = ViosndPcmCopyGain16(destination,
                                             source,
                                             copyLength,
                                             VIOSND_CAPTURE_SOFTWARE_GAIN);
#endif

            if (bytesRead < m_PacketSize) {
                RtlZeroMemory(destination + bytesRead, m_PacketSize - bytesRead);
            }

            m_PacketNumber = packetNumber + 1;
            m_Position += m_PacketSize;
            m_OutstandingWrites = m_PacketNumber - m_NextReadPacket;
            if (m_OutstandingWrites > m_NotificationCount) {
                ULONG droppedPackets = m_OutstandingWrites - m_NotificationCount;

                m_NextReadPacket += droppedPackets;
                m_OutstandingWrites = m_NotificationCount;
                m_CaptureOverflows += droppedPackets;
            }

            if ((packetNumber & 0x7f) == 0 || bytesRead == 0 || latencyBytes > m_PacketSize * 4) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture complete packet=%u bytes=%u peak=%u/%u gain=%u gate=%u pos=%llu latency=%u inFlight=%u ready=%u nextRead=%u overflow=%u\n",
                           m_StreamId,
                           packetNumber,
                           bytesRead,
                           inputPeak,
                           outputPeak,
                           VIOSND_CAPTURE_SOFTWARE_GAIN,
                           VIOSND_CAPTURE_NOISE_GATE_PEAK,
                           m_Position,
                           latencyBytes,
                           m_CaptureInFlight,
                           m_OutstandingWrites,
                           m_NextReadPacket,
                           m_CaptureOverflows);
            }

            if (m_NotificationEvent != NULL) {
                KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
            }
        } else {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u reclaim capture packet failed 0x%08x inFlight=%u\n",
                       m_StreamId,
                       status,
                       m_CaptureInFlight);
        }

        if (io != NULL && m_CaptureIoFreeCount < VIOSND_CAPTURE_IO_POOL_SIZE) {
            m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
        } else {
            ViosndFreeReadPcmIo(io);
        }
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::StartRenderWorker()
{
    HANDLE threadHandle = NULL;
    NTSTATUS status;

    if (m_WorkerStarted) {
        return STATUS_SUCCESS;
    }

    status = m_Capture ? AllocateCaptureIoPool() : AllocateRenderIoPool();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeClearEvent(&m_StopEvent);
    KeClearEvent(&m_KickEvent);
    AddRef();
    status = PsCreateSystemThread(&threadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  ViosndRenderThread,
                                  this);
    if (!NT_SUCCESS(status)) {
        Release();
        return status;
    }

    status = ObReferenceObjectByHandle(threadHandle,
                                       SYNCHRONIZE,
                                       NULL,
                                       KernelMode,
                                       &m_ThreadObject,
                                       NULL);
    ZwClose(threadHandle);
    if (!NT_SUCCESS(status)) {
        KeSetEvent(&m_StopEvent, IO_NO_INCREMENT, FALSE);
        return status;
    }

    m_WorkerStarted = TRUE;
    return STATUS_SUCCESS;
}

BOOLEAN
CViosndMiniportWaveRTStream::StopRenderWorker()
{
    LARGE_INTEGER timeout;
    PVOID threadObject = m_ThreadObject;
    NTSTATUS waitStatus;

    if (threadObject == NULL) {
        m_WorkerStarted = FALSE;
        return TRUE;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s stop worker begin outstanding=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               m_OutstandingWrites,
               m_NextSubmitPacket,
               m_LastOsWritePacket);
    KeSetEvent(&m_StopEvent, IO_NO_INCREMENT, FALSE);

    timeout.QuadPart = -(10LL * 1000LL * VIOSND_CAPTURE_STOP_TIMEOUT_MS);
    waitStatus = KeWaitForSingleObject(threadObject,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       m_Capture ? &timeout : NULL);
    if (waitStatus == STATUS_TIMEOUT) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u capture stop worker timeout; capture disabled outstanding=%u next=%u inFlight=%u\n",
                   m_StreamId,
                   m_OutstandingWrites,
                   m_NextSubmitPacket,
                   m_CaptureInFlight);
        InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
        ObDereferenceObject(threadObject);
        m_ThreadObject = NULL;
        m_WorkerStarted = FALSE;
        return FALSE;
    }

    ObDereferenceObject(threadObject);
    m_ThreadObject = NULL;
    m_WorkerStarted = FALSE;
    if (m_Capture) {
        FreeCaptureIoPool();
    } else {
        FreeRenderIoPool();
    }
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s stop worker done outstanding=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               m_OutstandingWrites,
               m_NextSubmitPacket,
               m_LastOsWritePacket);
    return TRUE;
}

ULONGLONG
CViosndMiniportWaveRTStream::GetRenderClockPosition()
{
    LARGE_INTEGER qpc;

    if (m_Capture ||
        m_RunStartQpc == 0 ||
        m_QpcFrequency <= 0) {
        return m_Position;
    }

    qpc = KeQueryPerformanceCounter(NULL);
    return m_RunStartPosition +
           ViosndRenderBytesFromQpc(qpc.QuadPart - m_RunStartQpc,
                                    m_QpcFrequency);
}

VOID
CViosndMiniportWaveRTStream::RenderWorkerLoop()
{
    LARGE_INTEGER interval;
    ULONG submittedSinceLog = 0;
    ULONG lastCompletedPackets = 0;
    ULONG targetOutstanding;
    PVOID waitObjects[2];

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    interval.QuadPart = -(10LL * VIOSND_RENDER_POLL_INTERVAL_US);
    waitObjects[0] = &m_StopEvent;
    waitObjects[1] = &m_KickEvent;

    if (m_Capture) {
        CaptureWorkerLoop();
        return;
    }

    for (;;) {
        NTSTATUS waitStatus;

        waitStatus = KeWaitForMultipleObjects(SIZEOF_ARRAY(waitObjects),
                                              waitObjects,
                                              WaitAny,
                                              Executive,
                                              KernelMode,
                                              FALSE,
                                              &interval,
                                              NULL);
        if (waitStatus == STATUS_WAIT_0) {
            break;
        }

        ViosndPollEvents(m_Device);
        ReclaimRenderPackets(&submittedSinceLog);
        if (m_PacketNumber != lastCompletedPackets) {
            lastCompletedPackets = m_PacketNumber;
            m_RenderStallLoops = 0;
        } else if (m_State == KSSTATE_RUN && m_OutstandingWrites != 0) {
            m_RenderStallLoops++;
            if ((m_RenderStallLoops % VIOSND_COMPLETION_STALL_LOG_LOOPS) == 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u render no completion loops=%u outstanding=%u next=%u last=%u pos=%llu\n",
                           m_StreamId,
                           m_RenderStallLoops,
                           m_OutstandingWrites,
                           m_NextSubmitPacket,
                           m_LastOsWritePacket,
                           m_Position);
            }
        }

        if (m_State != KSSTATE_RUN) {
            continue;
        }

        if (m_Buffer == NULL ||
            m_PacketSize == 0) {
            continue;
        }

        targetOutstanding = m_LastOsWritePacket == MAXULONG ?
                                ViosndCyclicTargetOutstandingPackets(m_NotificationCount) :
                                ViosndTargetOutstandingPackets(m_NotificationCount);
        while (m_State == KSSTATE_RUN &&
               m_OutstandingWrites < targetOutstanding) {
            ULONG packetLength = m_PacketSize;
            NTSTATUS status;

            if (ViosndHasWritableRenderPacket(m_NextSubmitPacket, m_LastOsWritePacket)) {
                if (m_EosPacketNumber == m_NextSubmitPacket) {
                    packetLength = m_EosPacketLength;
                }

                status = SubmitRenderPacket(m_NextSubmitPacket, packetLength);
            } else if (m_LastOsWritePacket == MAXULONG) {
                if (m_RenderPrerollPackets != 0) {
                    status = SubmitRenderSilencePacket(m_NextSubmitPacket);
                    if (NT_SUCCESS(status)) {
                        m_RenderPrerollPackets--;
                    }
                } else {
                    status = SubmitRenderPacket(m_NextSubmitPacket, m_PacketSize);
                }
                if (NT_SUCCESS(status)) {
                    m_RenderFallbackPackets++;
                    if ((m_RenderFallbackPackets & VIOSND_PERIODIC_LOG_MASK) == 1) {
                        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                                   DPFLTR_ERROR_LEVEL,
                                   "viosnd: stream %u render cyclic packet=%u outstanding=%u total=%u\n",
                                   m_StreamId,
                                   m_NextSubmitPacket - 1,
                                   m_OutstandingWrites,
                                   m_RenderFallbackPackets);
                    }
                }
            } else {
                if (m_OutstandingWrites == 0) {
                    m_RenderNotReadyLoops++;
                    if ((m_RenderNotReadyLoops & 0x7f) == 1) {
                        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                                   DPFLTR_ERROR_LEVEL,
                                   "viosnd: stream %u render packet not ready next=%u last=%u loops=%u\n",
                                   m_StreamId,
                                   m_NextSubmitPacket,
                                   m_LastOsWritePacket,
                                   m_RenderNotReadyLoops);
                    }
                }
                break;
            }

            if (!NT_SUCCESS(status)) {
                break;
            }
        }
    }

    while (m_OutstandingWrites != 0) {
        ReclaimRenderPackets(&submittedSinceLog);
        if (m_OutstandingWrites == 0) {
            break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}

VOID
CViosndMiniportWaveRTStream::CaptureWorkerLoop()
{
    LARGE_INTEGER interval;
    ULONG lastCompletedPackets = 0;
    ULONG targetOutstanding;

    interval.QuadPart = -(10LL * VIOSND_CAPTURE_POLL_INTERVAL_US);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture worker enter packet=%u size=%u notif=%u\n",
               m_StreamId,
               m_NextSubmitPacket,
               m_PacketSize,
               m_NotificationCount);

    targetOutstanding = min(VIOSND_CAPTURE_TARGET_OUTSTANDING_PACKETS,
                            min(m_NotificationCount, VIOSND_CAPTURE_IO_POOL_SIZE));
    targetOutstanding = max(targetOutstanding, 1u);

    while (m_State == KSSTATE_RUN) {
        NTSTATUS waitStatus;

        waitStatus = KeWaitForSingleObject(&m_StopEvent,
                                           Executive,
                                           KernelMode,
                                           FALSE,
                                           &interval);
        if (waitStatus == STATUS_SUCCESS) {
            break;
        }

        ReclaimCapturePackets();
        if (m_PacketNumber != lastCompletedPackets) {
            lastCompletedPackets = m_PacketNumber;
            m_CaptureStallLoops = 0;
        } else if (m_State == KSSTATE_RUN && m_CaptureInFlight != 0) {
            m_CaptureStallLoops++;
            if ((m_CaptureStallLoops % VIOSND_COMPLETION_STALL_LOG_LOOPS) == 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture no completion loops=%u inFlight=%u next=%u pos=%llu\n",
                           m_StreamId,
                           m_CaptureStallLoops,
                           m_CaptureInFlight,
                           m_NextSubmitPacket,
                           m_Position);
            }
        }

        if (m_State != KSSTATE_RUN ||
            m_Buffer == NULL ||
            m_PacketSize == 0 ||
            m_NotificationCount == 0) {
            continue;
        }

        while (m_State == KSSTATE_RUN && m_CaptureInFlight < targetOutstanding) {
            NTSTATUS status = SubmitCapturePacket();

            if (!NT_SUCCESS(status)) {
                if (status != STATUS_DEVICE_BUSY) {
                    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                               DPFLTR_ERROR_LEVEL,
                               "viosnd: stream %u capture submit failed 0x%08x next=%u inFlight=%u\n",
                               m_StreamId,
                               status,
                               m_NextSubmitPacket,
                               m_CaptureInFlight);
                }
                break;
            }
        }
    }

    for (ULONG i = 0; i < VIOSND_CAPTURE_STOP_RECLAIM_POLLS && m_CaptureInFlight != 0; ++i) {
        ReclaimCapturePackets();
        if (m_CaptureInFlight == 0) {
            break;
        }
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture worker exit packet=%u position=%llu state=%u inFlight=%u\n",
               m_StreamId,
               m_NextSubmitPacket,
               m_Position,
               m_State,
               m_CaptureInFlight);
}

static VOID
ViosndRenderThread(_In_ PVOID Context)
{
    CViosndMiniportWaveRTStream *stream = (CViosndMiniportWaveRTStream *)Context;

    stream->RenderWorkerLoop();
    stream->Release();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    NTSTATUS status = STATUS_SUCCESS;
    KSSTATE oldState = m_State;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s SetState begin old=%u new=%u worker=%u outstanding=%u packet=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               oldState,
               State,
               m_WorkerStarted,
               m_OutstandingWrites,
               m_PacketNumber,
               m_NextSubmitPacket,
               m_LastOsWritePacket);

    if (State == KSSTATE_RUN && oldState != KSSTATE_RUN) {
        if (!m_Capture) {
            LARGE_INTEGER frequency;
            LARGE_INTEGER qpc;

            m_RenderUnderruns = 0;
            m_RenderFallbackPackets = 0;
            m_RenderPrerollPackets = oldState == KSSTATE_STOP ?
                                         VIOSND_RENDER_CYCLIC_PREROLL_PACKETS :
                                         0;
            m_RenderStallLoops = 0;
            m_RenderNotReadyLoops = 0;
            qpc = KeQueryPerformanceCounter(&frequency);
            m_RunStartPosition = m_Position;
            m_RunStartQpc = qpc.QuadPart;
            m_QpcFrequency = frequency.QuadPart;
            status = ViosndStartPcm(m_Device, m_StreamId);
            if (NT_SUCCESS(status)) {
                m_State = State;
                status = StartRenderWorker();
                if (!NT_SUCCESS(status)) {
                    ViosndStopPcm(m_Device, m_StreamId);
                    m_State = oldState;
                }
            }
            if (!NT_SUCCESS(status)) {
                m_RunStartPosition = 0;
                m_RunStartQpc = 0;
                m_QpcFrequency = 0;
                m_State = oldState;
            }
        } else {
            if (InterlockedCompareExchange((volatile LONG *)&ViosndCaptureFaulted, 0, 0) != 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture start rejected; capture faulted\n",
                           m_StreamId);
                return STATUS_DEVICE_NOT_READY;
            }
            m_PacketNumber = 0;
            m_NextSubmitPacket = 0;
            m_NextReadPacket = 0;
            m_OutstandingWrites = 0;
            m_CaptureInFlight = 0;
            m_CaptureOverflows = 0;
            m_CaptureStallLoops = 0;
            m_CapturePositionQueries = 0;
            m_CaptureReadPackets = 0;
            m_Position = 0;
            status = ViosndStartPcm(m_Device, m_StreamId);
            if (NT_SUCCESS(status)) {
                m_State = State;
                status = StartRenderWorker();
                if (!NT_SUCCESS(status)) {
                    ViosndStopPcm(m_Device, m_StreamId);
                    m_State = oldState;
                }
            } else {
                m_State = oldState;
            }
        }
    } else if (State != KSSTATE_RUN && oldState == KSSTATE_RUN) {
        m_State = State;
        if (m_Capture) {
            NTSTATUS releaseStatus;
            NTSTATUS prepareStatus = STATUS_DEVICE_NOT_READY;
            ULONG detached = 0;
            BOOLEAN workerStopped = TRUE;

            if (m_WorkerStarted) {
                workerStopped = StopRenderWorker();
            }

            /*
             * QEMU's input STOP path can block in AUD_set_active_in() while RX
             * buffers are still queued. RELEASE is required to flush pending
             * I/O, so use it as the capture quiesce operation and then prepare
             * the stream for the next RUN.
             */
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture release pcm begin workerStopped=%u inFlight=%u\n",
                       m_StreamId,
                       workerStopped,
                       m_CaptureInFlight);
            releaseStatus = ViosndReleasePcm(m_Device, m_StreamId);
            if (NT_SUCCESS(releaseStatus)) {
                ReclaimCapturePackets();
                detached = ViosndDetachUnusedReadPcm(m_Device);
                m_CaptureInFlight = 0;
                m_OutstandingWrites = 0;
                FreeCaptureIoPool();
                prepareStatus = ViosndConfigureDefaultPcm(m_Device, m_StreamId);
            } else {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            if (!workerStopped) {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            if (workerStopped &&
                NT_SUCCESS(releaseStatus) &&
                !NT_SUCCESS(prepareStatus)) {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture release/drain release=0x%08x detached=%u prepare=0x%08x workerStopped=%u faulted=%ld\n",
                       m_StreamId,
                       releaseStatus,
                       detached,
                       prepareStatus,
                       workerStopped,
                       InterlockedCompareExchange((volatile LONG *)&ViosndCaptureFaulted, 0, 0));
            status = STATUS_SUCCESS;
        } else {
            if (m_WorkerStarted) {
                StopRenderWorker();
            }
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render stop pcm begin\n",
                       m_StreamId);
            status = ViosndStopPcm(m_Device, m_StreamId);
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render stop pcm done 0x%08x\n",
                       m_StreamId,
                       status);
            if (NT_SUCCESS(status)) {
                m_Position = GetRenderClockPosition();
                m_RunStartPosition = 0;
                m_RunStartQpc = 0;
                m_QpcFrequency = 0;
            }
        }
        if (!NT_SUCCESS(status)) {
            m_State = oldState;
        } else if (State == KSSTATE_STOP) {
            m_PacketNumber = 0;
            m_LastOsWritePacket = MAXULONG;
            m_NextSubmitPacket = 0;
            m_NextReadPacket = 0;
            m_EosPacketNumber = MAXULONG;
            m_EosPacketLength = 0;
            m_OutstandingWrites = 0;
            m_RenderUnderruns = 0;
            m_RenderFallbackPackets = 0;
            m_RenderPrerollPackets = 0;
            m_RenderStallLoops = 0;
            m_CaptureStallLoops = 0;
            m_RenderNotReadyLoops = 0;
            m_RunStartPosition = 0;
            m_RunStartQpc = 0;
            m_QpcFrequency = 0;
            if (m_Capture && m_CaptureInFlight != 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture stopped with in-flight=%u\n",
                           m_StreamId,
                           m_CaptureInFlight);
            } else {
                m_CaptureInFlight = 0;
            }
            m_Position = 0;
        }
    } else {
        m_State = State;
    }

    if (NT_SUCCESS(status)) {
        if (State == KSSTATE_RUN && !m_Capture) {
            KeSetEvent(&m_KickEvent, IO_NO_INCREMENT, FALSE);
        }
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s state=%u lastWrite=%u nextSubmit=%u outstanding=%u worker=%u buf=%u packet=%u notif=%u\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   State,
                   m_LastOsWritePacket,
                   m_NextSubmitPacket,
                   m_OutstandingWrites,
                   m_WorkerStarted,
                   m_BufferSize,
                   m_PacketSize,
                   m_NotificationCount);
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s SetState(%u) failed 0x%08x\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   State,
                   status);
    }
    return status;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    ULONGLONG currentPosition;
    ULONGLONG playOffset;
    ULONGLONG writeLead;
    ULONG targetOutstanding;

    if (m_BufferSize == 0) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    if (m_Capture) {
        ULONGLONG recordOffset;

        recordOffset = m_Position % m_BufferSize;
        Position->PlayOffset = recordOffset;
        Position->WriteOffset = recordOffset;
        m_CapturePositionQueries++;
        if (m_CapturePositionQueries <= 8 ||
            (m_CapturePositionQueries & 0x7f) == 0) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture GetPosition count=%u posOffset=%llu pos=%llu inFlight=%u nextSubmit=%u\n",
                       m_StreamId,
                       m_CapturePositionQueries,
                       recordOffset,
                       m_Position,
                       m_CaptureInFlight,
                       m_NextSubmitPacket);
        }
        return STATUS_SUCCESS;
    }

    currentPosition = GetRenderClockPosition();
    playOffset = currentPosition % m_BufferSize;
    targetOutstanding = m_LastOsWritePacket == MAXULONG ?
                            ViosndCyclicTargetOutstandingPackets(m_NotificationCount) :
                            ViosndTargetOutstandingPackets(m_NotificationCount);
    writeLead = (ULONGLONG)m_PacketSize * max(targetOutstanding, 1u);
    Position->PlayOffset = playOffset;
    Position->WriteOffset = (playOffset + writeLead) % m_BufferSize;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS skip;
    ULONG allocationSize;

    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    skip.QuadPart = 0;
    allocationSize = RequestedSize;
    allocationSize = max(allocationSize, VIOSND_DEFAULT_BUFFER_BYTES);

    m_Mdl = MmAllocatePagesForMdlEx(low, high, skip, allocationSize, MmCached, 0);
    if (m_Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_Buffer = MmGetSystemAddressForMdlSafe(m_Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (m_Buffer == NULL) {
        FreeAudioBuffer(m_Mdl, allocationSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG allocatedSize = MmGetMdlByteCount(m_Mdl);
    RtlZeroMemory(m_Buffer, allocatedSize);
    m_BufferSize = ViosndUsableBufferSizeFromAllocatedSize(allocatedSize);
    m_NotificationCount = ViosndNotificationCountFromBufferSize(m_BufferSize);
    m_PacketSize = ViosndPacketSizeFromNotificationCount(m_BufferSize,
                                                         m_NotificationCount);
    *AudioBufferMdl = m_Mdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s AllocateAudioBuffer requested=%u allocated=%u actual=%u packet=%u notif=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               RequestedSize,
               allocatedSize,
               m_BufferSize,
               m_PacketSize,
               m_NotificationCount);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(BufferSize);

    if (AudioBufferMdl != NULL) {
        MmFreePagesFromMdl(AudioBufferMdl);
        ExFreePool(AudioBufferMdl);
    }

    if (AudioBufferMdl == m_Mdl) {
        FreeRenderIoPool();
        FreeCaptureIoPool();
        m_Mdl = NULL;
        m_Buffer = NULL;
        m_BufferSize = 0;
        m_NotificationCount = 0;
        m_PacketSize = 0;
        m_PacketNumber = 0;
        m_LastOsWritePacket = MAXULONG;
        m_NextSubmitPacket = 0;
        m_NextReadPacket = 0;
        m_EosPacketNumber = MAXULONG;
        m_EosPacketLength = 0;
        m_OutstandingWrites = 0;
        m_RenderUnderruns = 0;
        m_RenderFallbackPackets = 0;
        m_RenderPrerollPackets = 0;
        m_RenderStallLoops = 0;
        m_CaptureStallLoops = 0;
        m_RenderNotReadyLoops = 0;
        if (m_CaptureInFlight == 0) {
            m_CaptureOverflows = 0;
        }
        m_Position = 0;
    }
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *Latency)
{
    RtlZeroMemory(Latency, sizeof(*Latency));
    Latency->FifoSize = VIOSND_DEFAULT_PERIOD_BYTES;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    RtlZeroMemory(Register, sizeof(*Register));
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    RtlZeroMemory(Register, sizeof(*Register));
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::AllocateBufferWithNotification(
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    ULONG allocationRequest;
    ULONG requestedNotificationCount;
    ULONGLONG periodAlignedSize;

    requestedNotificationCount = NotificationCount != 0 ? NotificationCount :
                                                          (VIOSND_DEFAULT_BUFFER_BYTES /
                                                           VIOSND_DEFAULT_PERIOD_BYTES);
    requestedNotificationCount = max(requestedNotificationCount, 1u);
    periodAlignedSize = (ULONGLONG)requestedNotificationCount * VIOSND_DEFAULT_PERIOD_BYTES;
    if (periodAlignedSize > MAXULONG) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    allocationRequest = (ULONG)periodAlignedSize;

    NTSTATUS status = AllocateAudioBuffer(allocationRequest,
                                          AudioBufferMdl,
                                          ActualSize,
                                          OffsetFromFirstPage,
                                          CacheType);
    if (NT_SUCCESS(status)) {
        m_NotificationCount = ViosndNotificationCountFromBufferSize(m_BufferSize);
        m_PacketSize = ViosndPacketSizeFromNotificationCount(m_BufferSize,
                                                             m_NotificationCount);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s AllocateBufferWithNotification requested=%u allocatedRequest=%u actual=%u packet=%u notif=%u requestedNotif=%u fixedPeriod=%u\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   RequestedSize,
                   allocationRequest,
                   m_BufferSize,
                   m_PacketSize,
                   m_NotificationCount,
                   NotificationCount,
                   VIOSND_DEFAULT_PERIOD_BYTES);
    }
    return status;
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::FreeBufferWithNotification(_In_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    FreeAudioBuffer(AudioBufferMdl, BufferSize);
    m_NotificationCount = 0;
    m_PacketSize = 0;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    m_NotificationEvent = NotificationEvent;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    if (m_NotificationEvent == NotificationEvent) {
        m_NotificationEvent = NULL;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetReadPacket(
    _Out_ ULONG *PacketNumber,
    _Out_ DWORD *Flags,
    _Out_ ULONG64 *PerformanceCounterValue,
    _Out_ BOOL *MoreData)
{
    LARGE_INTEGER qpc;

    if (!m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_State != KSSTATE_RUN || m_OutstandingWrites == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    *PacketNumber = m_NextReadPacket++;
    *Flags = 0;
    qpc = KeQueryPerformanceCounter(NULL);
    *PerformanceCounterValue = (ULONG64)qpc.QuadPart;
    m_OutstandingWrites--;
    *MoreData = m_OutstandingWrites != 0 ? TRUE : FALSE;
    m_CaptureReadPackets++;
    if (m_CaptureReadPackets <= 16 || ((*PacketNumber) & 0x7f) == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u capture GetReadPacket count=%u packet=%u ready=%u more=%u qpc=%llu\n",
                   m_StreamId,
                   m_CaptureReadPackets,
                   *PacketNumber,
                   m_OutstandingWrites,
                   *MoreData,
                   *PerformanceCounterValue);
    }
    if (m_NotificationEvent != NULL) {
        KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetWritePacket(
    _In_ ULONG PacketNumber,
    _In_ DWORD Flags,
    _In_ ULONG EosPacketLength)
{
    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_LastOsWritePacket == MAXULONG) {
        m_NextSubmitPacket = PacketNumber;
    }
    m_LastOsWritePacket = PacketNumber;

    if ((Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) != 0) {
        m_EosPacketNumber = PacketNumber;
        m_EosPacketLength = min(EosPacketLength, m_PacketSize);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render SetWritePacket EOS packet=%u eosLength=%u packetSize=%u outstanding=%u next=%u\n",
                   m_StreamId,
                   PacketNumber,
                   m_EosPacketLength,
                   m_PacketSize,
                   m_OutstandingWrites,
                   m_NextSubmitPacket);
    } else if (m_EosPacketNumber != MAXULONG &&
               ViosndPacketNumberLessOrEqual(m_EosPacketNumber, PacketNumber)) {
        m_EosPacketNumber = MAXULONG;
        m_EosPacketLength = 0;
    }

    if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 || m_OutstandingWrites == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render SetWritePacket packet=%u flags=0x%x last=%u next=%u outstanding=%u state=%u\n",
                   m_StreamId,
                   PacketNumber,
                   Flags,
                   m_LastOsWritePacket,
                   m_NextSubmitPacket,
                   m_OutstandingWrites,
                   m_State);
    }

    KeSetEvent(&m_KickEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetOutputStreamPresentationPosition(
    _Out_ KSAUDIO_PRESENTATION_POSITION *PresentationPosition)
{
    ULONGLONG currentPosition = m_Capture ? m_Position : GetRenderClockPosition();

    PresentationPosition->u64PositionInBlocks = currentPosition /
        ((VIOSND_DEFAULT_CHANNELS * VIOSND_DEFAULT_BITS_PER_SAMPLE) / 8);
    PresentationPosition->u64QPCPosition = KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPacketCount(_Out_ ULONG *PacketCount)
{
    *PacketCount = m_PacketNumber;
    return STATUS_SUCCESS;
}

CViosndMiniportWaveRT::CViosndMiniportWaveRT(
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture) :
    m_RefCount(1),
    m_Device(Device),
    m_StreamId(StreamId),
    m_Capture(Capture),
    m_Port(NULL)
{
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface)
{
    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRT)) {
        *Interface = (IMiniportWaveRT *)this;
    }

    if (*Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRT::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRT::Release()
{
    LONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        this->~CViosndMiniportWaveRT();
        ExFreePoolWithTag(this, VIOSND_POOL_TAG);
    }
    return (ULONG)count;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    *Description = m_Capture ? &ViosndCaptureFilterDescriptor : &ViosndRenderFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (OutputBufferLength == 0) {
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ViosndBuildDefaultFormat((PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(PortStream);

    if (Stream == NULL ||
        Pin != VIOSND_PIN_SYSTEM ||
        Capture != m_Capture ||
        !ViosndIsDefaultFormat(DataFormat)) {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID memory = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                               sizeof(CViosndMiniportWaveRTStream),
                                               VIOSND_POOL_TAG);
    if (memory == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Stream = new(memory) CViosndMiniportWaveRTStream(this, m_Device, m_StreamId, m_Capture);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = FALSE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->DmaWidth = Width32Bits;
    DeviceDescription->DmaSpeed = Compatible;
    DeviceDescription->MaximumLength = VIOSND_DEFAULT_BUFFER_BYTES;
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndCreateWaveRTMiniport(
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture,
    _Outptr_ PMINIPORT *Miniport)
{
    PVOID memory;

    *Miniport = NULL;
    memory = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                         sizeof(CViosndMiniportWaveRT),
                                         VIOSND_POOL_TAG);
    if (memory == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Miniport = new(memory) CViosndMiniportWaveRT(Device, StreamId, Capture);
    return STATUS_SUCCESS;
}


