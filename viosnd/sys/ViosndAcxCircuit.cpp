#include "ViosndAcx.h"

#define VIOSND_ACX_PIN_HOST 0u
#define VIOSND_ACX_PIN_BRIDGE 1u

static const GUID VIOSND_ACX_RENDER_COMPONENT_ID =
{ 0x8d6f2c1e, 0x6a47, 0x4dc0, { 0x98, 0x1b, 0x24, 0x7d, 0xa2, 0x1b, 0x0e, 0x11 } };

static const GUID VIOSND_ACX_CAPTURE_COMPONENT_ID =
{ 0xf12d8b9c, 0x4f6e, 0x46f4, { 0x8f, 0x57, 0x3d, 0x3c, 0x09, 0x6b, 0x8d, 0x42 } };

static EVT_ACX_CIRCUIT_CREATE_STREAM ViosndEvtCircuitCreateStream;
static EVT_ACX_STREAM_PREPARE_HARDWARE ViosndEvtStreamPrepareHardware;
static EVT_ACX_STREAM_RELEASE_HARDWARE ViosndEvtStreamReleaseHardware;
static EVT_ACX_STREAM_RUN ViosndEvtStreamRun;
static EVT_ACX_STREAM_PAUSE ViosndEvtStreamPause;
static EVT_ACX_STREAM_GET_HW_LATENCY ViosndEvtStreamGetHwLatency;
static EVT_ACX_STREAM_ALLOCATE_RTPACKETS ViosndEvtStreamAllocateRtPackets;
static EVT_ACX_STREAM_FREE_RTPACKETS ViosndEvtStreamFreeRtPackets;
static EVT_ACX_STREAM_SET_RENDER_PACKET ViosndEvtStreamSetRenderPacket;
static EVT_ACX_STREAM_GET_CURRENT_PACKET ViosndEvtStreamGetCurrentPacket;
static EVT_ACX_STREAM_GET_CAPTURE_PACKET ViosndEvtStreamGetCapturePacket;
static EVT_ACX_STREAM_GET_PRESENTATION_POSITION ViosndEvtStreamGetPresentationPosition;
static EVT_ACX_PIN_SET_DATAFORMAT ViosndEvtPinSetDataFormat;
static EVT_ACX_CIRCUIT_POWER_UP ViosndEvtCircuitPowerUp;
static EVT_ACX_CIRCUIT_POWER_DOWN ViosndEvtCircuitPowerDown;
static EVT_ACX_MUTE_ASSIGN_STATE ViosndEvtMuteAssignState;
static EVT_ACX_MUTE_RETRIEVE_STATE ViosndEvtMuteRetrieveState;
static EVT_ACX_VOLUME_ASSIGN_LEVEL ViosndEvtVolumeAssignLevel;
static EVT_ACX_RAMPED_VOLUME_ASSIGN_LEVEL ViosndEvtRampedVolumeAssignLevel;
static EVT_ACX_VOLUME_RETRIEVE_LEVEL ViosndEvtVolumeRetrieveLevel;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP ViosndEvtStreamCleanup;

typedef struct _VIOSND_ACX_MUTE_CONTEXT {
    ULONG State[VIOSND_DEFAULT_CHANNELS];
} VIOSND_ACX_MUTE_CONTEXT, *PVIOSND_ACX_MUTE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_MUTE_CONTEXT, ViosndGetMuteContext)

typedef struct _VIOSND_ACX_VOLUME_CONTEXT {
    LONG Level[VIOSND_DEFAULT_CHANNELS];
} VIOSND_ACX_VOLUME_CONTEXT, *PVIOSND_ACX_VOLUME_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_VOLUME_CONTEXT, ViosndGetVolumeContext)

typedef enum _VIOSND_ACX_PIN_ROLE {
    ViosndAcxPinRoleHost = 0,
    ViosndAcxPinRoleBridge = 1
} VIOSND_ACX_PIN_ROLE;

typedef struct _VIOSND_ACX_PIN_CONTEXT {
    VIOSND_ACX_PIN_ROLE Role;
} VIOSND_ACX_PIN_CONTEXT, *PVIOSND_ACX_PIN_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_PIN_CONTEXT, ViosndGetPinContext)

typedef struct _VIOSND_ACX_JACK_CONTEXT {
    ULONG Dummy;
} VIOSND_ACX_JACK_CONTEXT, *PVIOSND_ACX_JACK_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOSND_ACX_JACK_CONTEXT, ViosndGetJackContext)

static NTSTATUS
ViosndPrepareStreamPeriods(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static VOID
ViosndFreeStreamPeriods(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static VOID
ViosndFreeRtPacketStorage(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static NTSTATUS
ViosndConfigurePcmStream(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static NTSTATUS
ViosndSubmitRenderPacket(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG Packet);

static VOID
ViosndPumpRenderQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static VOID
ViosndClearRenderQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static NTSTATUS
ViosndSubmitCapturePeriod(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _Inout_ PVIOSND_ACX_PERIOD Period,
    _In_ ULONGLONG PacketNumber);

static VOID
ViosndDrainStreamQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext);

static BOOLEAN
ViosndStreamHasInFlight(
    _In_ PVIOSND_ACX_STREAM StreamContext);

static VOID
ViosndWaitForStreamIdle(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG TimeoutMs);

static VOID
ViosndStopStream(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ BOOLEAN Release);

static VIOSND_KSDATAFORMAT_WAVEFORMATEXTENSIBLE ViosndDefaultKsFormat;

static const LONG VIOSND_VOLUME_MINIMUM = -96 * 0x10000;
static const LONG VIOSND_VOLUME_MAXIMUM = 0;
static const ULONG VIOSND_VOLUME_STEPPING = 0x10000;
static const ULONG VIOSND_ALL_CHANNELS_ID = MAXULONG;

static NTSTATUS
ViosndEvtCircuitPowerUp(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX circuit power up circuit=%p previous=%u\n",
               Circuit,
               PreviousState);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtCircuitPowerDown(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX circuit power down circuit=%p target=%u\n",
               Circuit,
               TargetState);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtMuteAssignState(
    _In_ ACXMUTE Mute,
    _In_ ULONG Channel,
    _In_ ULONG State)
{
    PVIOSND_ACX_MUTE_CONTEXT context = ViosndGetMuteContext(Mute);

    if (Channel == VIOSND_ALL_CHANNELS_ID) {
        for (ULONG i = 0; i < VIOSND_DEFAULT_CHANNELS; ++i) {
            context->State[i] = State;
        }
    } else if (Channel < VIOSND_DEFAULT_CHANNELS) {
        context->State[Channel] = State;
    } else {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtMuteRetrieveState(
    _In_ ACXMUTE Mute,
    _In_ ULONG Channel,
    _Out_ ULONG *State)
{
    PVIOSND_ACX_MUTE_CONTEXT context = ViosndGetMuteContext(Mute);

    if (Channel == VIOSND_ALL_CHANNELS_ID) {
        *State = context->State[0];
    } else if (Channel < VIOSND_DEFAULT_CHANNELS) {
        *State = context->State[Channel];
    } else {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtVolumeAssignLevel(
    _In_ ACXVOLUME Volume,
    _In_ ULONG Channel,
    _In_ LONG VolumeLevel)
{
    PVIOSND_ACX_VOLUME_CONTEXT context = ViosndGetVolumeContext(Volume);

    if (VolumeLevel < VIOSND_VOLUME_MINIMUM || VolumeLevel > VIOSND_VOLUME_MAXIMUM) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Channel == VIOSND_ALL_CHANNELS_ID) {
        for (ULONG i = 0; i < VIOSND_DEFAULT_CHANNELS; ++i) {
            context->Level[i] = VolumeLevel;
        }
    } else if (Channel < VIOSND_DEFAULT_CHANNELS) {
        context->Level[Channel] = VolumeLevel;
    } else {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtRampedVolumeAssignLevel(
    _In_ ACXVOLUME Volume,
    _In_ ULONG Channel,
    _In_ LONG VolumeLevel,
    _In_ ACX_VOLUME_CURVE_TYPE CurveType,
    _In_ ULONGLONG CurveDuration)
{
    UNREFERENCED_PARAMETER(CurveType);
    UNREFERENCED_PARAMETER(CurveDuration);
    return ViosndEvtVolumeAssignLevel(Volume, Channel, VolumeLevel);
}

static NTSTATUS
ViosndEvtVolumeRetrieveLevel(
    _In_ ACXVOLUME Volume,
    _In_ ULONG Channel,
    _Out_ LONG *VolumeLevel)
{
    PVIOSND_ACX_VOLUME_CONTEXT context = ViosndGetVolumeContext(Volume);

    if (Channel == VIOSND_ALL_CHANNELS_ID) {
        *VolumeLevel = context->Level[0];
    } else if (Channel < VIOSND_DEFAULT_CHANNELS) {
        *VolumeLevel = context->Level[Channel];
    } else {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtPinSetDataFormat(
    _In_ ACXPIN Pin,
    _In_ ACXDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(DataFormat);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX pin set format pin=%u\n",
               AcxPinGetId(Pin));
    return STATUS_SUCCESS;
}

static VOID
ViosndBuildDefaultFormat(
    _Out_ PVIOSND_KSDATAFORMAT_WAVEFORMATEXTENSIBLE Format)
{
    RtlZeroMemory(Format, sizeof(*Format));
    Format->DataFormat.FormatSize = sizeof(*Format);
    Format->DataFormat.SampleSize = 0;
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
        Format->WaveFormatExt.Format.nSamplesPerSec *
        Format->WaveFormatExt.Format.nBlockAlign;
    Format->WaveFormatExt.Format.cbSize =
        sizeof(VIOSND_WAVEFORMATEXTENSIBLE) - sizeof(VIOSND_WAVEFORMATEX);
    Format->WaveFormatExt.Samples.wValidBitsPerSample =
        VIOSND_DEFAULT_BITS_PER_SAMPLE;
    Format->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    Format->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

static NTSTATUS
ViosndAssignDefaultFormat(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ ACXPIN Pin)
{
    NTSTATUS status;
    ACXDATAFORMAT dataFormat;
    ACXDATAFORMATLIST rawFormatList;
    ACX_DATAFORMAT_CONFIG formatConfig;
    WDF_OBJECT_ATTRIBUTES attributes;

    ViosndBuildDefaultFormat(&ViosndDefaultKsFormat);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Circuit;
    ACX_DATAFORMAT_CONFIG_INIT_KS(&formatConfig, &ViosndDefaultKsFormat);
    status = AcxDataFormatCreate(Device, &attributes, &formatConfig, &dataFormat);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateCircuit.DataFormatCreate", status);
        return status;
    }

    rawFormatList = AcxPinGetRawDataFormatList(Pin);
    if (rawFormatList == NULL) {
        ViosndAcxWriteDiag(Device, L"CreateCircuit.RawFormatListMissing", STATUS_NOT_FOUND);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = AcxDataFormatListAddDataFormat(rawFormatList, dataFormat);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateCircuit.RawFormatListAdd", status);
        return status;
    }

    ViosndAcxWriteDiag(Device, L"CreateCircuit.RawFormatListAdd", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndAddBridgeJack(
    _In_ WDFDEVICE Device,
    _In_ ACXPIN BridgePin)
{
    NTSTATUS status;
    ACX_JACK_CONFIG jackConfig;
    ACXJACK jack;
    WDF_OBJECT_ATTRIBUTES attributes;
    PVIOSND_ACX_JACK_CONTEXT jackContext;

    ACX_JACK_CONFIG_INIT(&jackConfig);
    jackConfig.Description.ChannelMapping = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    jackConfig.Description.Color = 0;
    jackConfig.Description.ConnectionType = AcxConnTypeAtapiInternal;
    jackConfig.Description.GeoLocation = AcxGeoLocFront;
    jackConfig.Description.GenLocation = AcxGenLocPrimaryBox;
    jackConfig.Description.PortConnection = AcxPortConnIntegratedDevice;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VIOSND_ACX_JACK_CONTEXT);
    attributes.ParentObject = BridgePin;
    status = AcxJackCreate(BridgePin, &attributes, &jackConfig, &jack);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateCircuit.JackCreate", status);
        return STATUS_SUCCESS;
    }

    jackContext = ViosndGetJackContext(jack);
    if (jackContext != NULL) {
        jackContext->Dummy = 0;
    }

    status = AcxPinAddJacks(BridgePin, &jack, 1);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateCircuit.PinAddJacks", status);
        return STATUS_SUCCESS;
    }

    ViosndAcxWriteDiag(Device, L"CreateCircuit.PinAddJacks", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndAddControlElements(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ BOOLEAN Capture)
{
    UNREFERENCED_PARAMETER(Circuit);

    ViosndAcxWriteDiag(Device,
                       Capture ? L"CreateCircuit.CaptureControlsSkipped" :
                                 L"CreateCircuit.RenderControlsSkipped",
                       STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndAcxCreateCircuit(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Capture,
    _In_ ULONG StreamId,
    _Out_ ACXCIRCUIT *Circuit)
{
    NTSTATUS status;
    PACXCIRCUIT_INIT circuitInit;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_CIRCUIT_PNPPOWER_CALLBACKS powerCallbacks;
    ACXCIRCUIT circuit = NULL;
    ACXPIN pins[2] = {};
    ACX_PIN_CONFIG pinConfig;
    ACX_PIN_CALLBACKS pinCallbacks;
    PVIOSND_ACX_PIN_CONTEXT pinContext;
    UNICODE_STRING circuitName;

    *Circuit = NULL;
    circuitInit = AcxCircuitInitAllocate(Device);
    if (circuitInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    AcxCircuitInitSetComponentId(circuitInit,
                                 Capture ? &VIOSND_ACX_CAPTURE_COMPONENT_ID :
                                           &VIOSND_ACX_RENDER_COMPONENT_ID);
    ACX_CIRCUIT_PNPPOWER_CALLBACKS_INIT(&powerCallbacks);
    powerCallbacks.EvtAcxCircuitPowerUp = ViosndEvtCircuitPowerUp;
    powerCallbacks.EvtAcxCircuitPowerDown = ViosndEvtCircuitPowerDown;
    AcxCircuitInitSetAcxCircuitPnpPowerCallbacks(circuitInit, &powerCallbacks);

    if (Capture) {
        RtlInitUnicodeString(&circuitName, L"Microphone0");
        AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeCapture);
    } else {
        RtlInitUnicodeString(&circuitName, L"Speaker0");
        AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeRender);
    }

    status = AcxCircuitInitAssignName(circuitInit, &circuitName);
    if (NT_SUCCESS(status)) {
        status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit,
                                                             ViosndEvtCircuitCreateStream);
    }
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureInit" : L"CreateCircuit.RenderInit",
                           status);
        AcxCircuitInitFree(circuitInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = AcxCircuitCreate(Device, &attributes, &circuitInit, &circuit);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureCircuitCreate" : L"CreateCircuit.RenderCircuitCreate",
                           status);
        AcxCircuitInitFree(circuitInit);
        return status;
    }

    status = ViosndAddControlElements(Device, circuit, Capture);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ACX_PIN_CALLBACKS_INIT(&pinCallbacks);
    pinCallbacks.EvtAcxPinSetDataFormat = ViosndEvtPinSetDataFormat;

    ACX_PIN_CONFIG_INIT(&pinConfig);
    pinConfig.Type = Capture ? AcxPinTypeSource : AcxPinTypeSink;
    pinConfig.Communication = AcxPinCommunicationSink;
    pinConfig.Category = &KSCATEGORY_AUDIO;
    pinConfig.PinCallbacks = &pinCallbacks;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VIOSND_ACX_PIN_CONTEXT);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinConfig, &pins[0]);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureHostPinCreate" : L"CreateCircuit.RenderHostPinCreate",
                           status);
        return status;
    }
    pinContext = ViosndGetPinContext(pins[0]);
    pinContext->Role = ViosndAcxPinRoleHost;

    status = ViosndAssignDefaultFormat(Device, circuit, pins[0]);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureAssignFormat" : L"CreateCircuit.RenderAssignFormat",
                           status);
        return status;
    }

    ACX_PIN_CONFIG_INIT(&pinConfig);
    pinConfig.Type = Capture ? AcxPinTypeSink : AcxPinTypeSource;
    pinConfig.Communication = AcxPinCommunicationNone;
    pinConfig.Category = Capture ? &KSNODETYPE_MICROPHONE : &KSNODETYPE_SPEAKER;
    pinConfig.PinCallbacks = &pinCallbacks;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VIOSND_ACX_PIN_CONTEXT);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinConfig, &pins[1]);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureBridgePinCreate" : L"CreateCircuit.RenderBridgePinCreate",
                           status);
        return status;
    }
    pinContext = ViosndGetPinContext(pins[1]);
    pinContext->Role = ViosndAcxPinRoleBridge;

    status = ViosndAddBridgeJack(Device, pins[1]);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureBridgeJack" : L"CreateCircuit.RenderBridgeJack",
                           status);
        return status;
    }

    status = AcxCircuitAddPins(circuit, pins, ARRAYSIZE(pins));
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device,
                           Capture ? L"CreateCircuit.CaptureAddPins" : L"CreateCircuit.RenderAddPins",
                           status);
        return status;
    }

    *Circuit = circuit;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX circuit created capture=%u stream=%u hostId=%u bridgeId=%u hostType=%u bridgeType=%u\n",
               Capture,
               StreamId,
               AcxPinGetId(pins[0]),
               AcxPinGetId(pins[1]),
               Capture ? AcxPinTypeSource : AcxPinTypeSink,
               Capture ? AcxPinTypeSink : AcxPinTypeSource);
    return STATUS_SUCCESS;
}

static ULONG
ViosndClampPeriodCount(
    _In_ ULONG PacketCount)
{
    if (PacketCount < VIOSND_MIN_PERIOD_COUNT) {
        return 4;
    }
    if (PacketCount > VIOSND_MAX_PERIOD_COUNT) {
        return VIOSND_MAX_PERIOD_COUNT;
    }
    return PacketCount;
}

static PUCHAR
ViosndGetPacketVa(
    _In_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG Packet)
{
    if (StreamContext->PacketBuffers == NULL ||
        StreamContext->PacketCount == 0 ||
        StreamContext->PacketSize == 0) {
        return NULL;
    }

    ULONG packetIndex = Packet % StreamContext->PacketCount;
    if (StreamContext->PacketBuffers[packetIndex] == NULL) {
        return NULL;
    }
    return (PUCHAR)StreamContext->PacketBuffers[packetIndex] +
           StreamContext->Packets[packetIndex].RtPacketOffset;
}

static NTSTATUS
ViosndPrepareStreamPeriods(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    ULONG periodCount;
    ULONG periodBytes;
    PVIOSND_ACX_DEVICE device = StreamContext->Device;

    if (StreamContext->Periods != NULL) {
        return STATUS_SUCCESS;
    }

    periodCount = ViosndClampPeriodCount(StreamContext->PacketCount);
    periodBytes = StreamContext->PacketSize != 0 ?
                  StreamContext->PacketSize :
                  VIOSND_DEFAULT_PERIOD_BYTES;
    if (periodBytes == 0 || periodBytes > 256u * 1024u) {
        return STATUS_INVALID_PARAMETER;
    }

    StreamContext->Periods =
        (PVIOSND_ACX_PERIOD)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                        periodCount * sizeof(VIOSND_ACX_PERIOD),
                                                        VIOSND_POOL_TAG);
    if (StreamContext->Periods == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(StreamContext->Periods, periodCount * sizeof(VIOSND_ACX_PERIOD));

    StreamContext->PeriodCount = periodCount;
    StreamContext->PeriodBytes = periodBytes;
    StreamContext->BufferBytes = periodBytes * periodCount;

    for (ULONG i = 0; i < periodCount; ++i) {
        PVIOSND_ACX_PERIOD period = &StreamContext->Periods[i];

        period->Index = i;
        period->Length = periodBytes;
        period->RequestLength = StreamContext->Capture ?
                                sizeof(VIRTIO_SND_PCM_XFER) :
                                sizeof(VIRTIO_SND_PCM_XFER) + periodBytes;
        period->RequestVa = ViosndAcxAllocDma(device,
                                              period->RequestLength,
                                              &period->RequestPa);
        period->StatusVa =
            (VIRTIO_SND_PCM_STATUS *)ViosndAcxAllocDma(device,
                                                       sizeof(VIRTIO_SND_PCM_STATUS),
                                                       &period->StatusPa);
        if (StreamContext->Capture) {
            period->AudioVa = ViosndAcxAllocDma(device,
                                                periodBytes,
                                                &period->AudioPa);
        }

        if (period->RequestVa == NULL ||
            period->StatusVa == NULL ||
            (StreamContext->Capture && period->AudioVa == NULL)) {
            ViosndFreeStreamPeriods(StreamContext);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    return STATUS_SUCCESS;
}

static VOID
ViosndFreeStreamPeriods(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    if (StreamContext->Periods == NULL) {
        return;
    }

    for (ULONG i = 0; i < StreamContext->PeriodCount; ++i) {
        PVIOSND_ACX_PERIOD period = &StreamContext->Periods[i];
        ViosndAcxFreeDma(StreamContext->Device, period->RequestVa);
        ViosndAcxFreeDma(StreamContext->Device, period->AudioVa);
        ViosndAcxFreeDma(StreamContext->Device, period->StatusVa);
    }

    ExFreePoolWithTag(StreamContext->Periods, VIOSND_POOL_TAG);
    StreamContext->Periods = NULL;
    StreamContext->PeriodCount = 0;
    StreamContext->PeriodBytes = 0;
    StreamContext->BufferBytes = 0;
}

static VOID
ViosndFreeRtPacketStorage(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    if (StreamContext == NULL) {
        return;
    }

    if (StreamContext->PacketMdls != NULL) {
        for (ULONG i = 0; i < StreamContext->PacketCount; ++i) {
            if (StreamContext->PacketMdls[i] != NULL) {
                IoFreeMdl(StreamContext->PacketMdls[i]);
                StreamContext->PacketMdls[i] = NULL;
            }
        }
        ExFreePoolWithTag(StreamContext->PacketMdls, VIOSND_POOL_TAG);
        StreamContext->PacketMdls = NULL;
    }

    if (StreamContext->PacketBuffers != NULL) {
        for (ULONG i = 0; i < StreamContext->PacketCount; ++i) {
            if (StreamContext->PacketBuffers[i] != NULL) {
                ExFreePoolWithTag(StreamContext->PacketBuffers[i], VIOSND_POOL_TAG);
                StreamContext->PacketBuffers[i] = NULL;
            }
        }
        ExFreePoolWithTag(StreamContext->PacketBuffers, VIOSND_POOL_TAG);
        StreamContext->PacketBuffers = NULL;
    }

    if (StreamContext->Packets != NULL) {
        ExFreePoolWithTag(StreamContext->Packets, VIOSND_POOL_TAG);
        StreamContext->Packets = NULL;
    }

    if (StreamContext->RenderReadyQueue != NULL) {
        ExFreePoolWithTag(StreamContext->RenderReadyQueue, VIOSND_POOL_TAG);
        StreamContext->RenderReadyQueue = NULL;
    }

    StreamContext->PacketCount = 0;
    StreamContext->PacketSize = 0;
    StreamContext->PacketAllocBytes = 0;
    StreamContext->FirstPacketOffset = 0;
    StreamContext->RenderReadyCapacity = 0;
    StreamContext->RenderReadyHead = 0;
    StreamContext->RenderReadyTail = 0;
    StreamContext->RenderReadyCount = 0;
}

static NTSTATUS
ViosndConfigurePcmStream(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    VIOSND_PCM_FORMAT format;
    VIRTIO_SND_PCM_SET_PARAMS params;
    NTSTATUS status;

    if (StreamContext->Prepared) {
        return STATUS_SUCCESS;
    }

    status = ViosndPrepareStreamPeriods(StreamContext);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: prepare periods failed stream=%u capture=%u status=0x%08x packetCount=%u packetSize=%u\n",
                   StreamContext->StreamId,
                   StreamContext->Capture,
                   status,
                   StreamContext->PacketCount,
                   StreamContext->PacketSize);
        return status;
    }

    ViosndGetDefaultPcmFormat(&format);
    format.PeriodBytes = StreamContext->PeriodBytes;
    format.BufferBytes = StreamContext->BufferBytes;
    ViosndBuildSetParams(&params, StreamContext->StreamId, &format);

    status = ViosndAcxControlStatusCommand(StreamContext->Device,
                                           &params,
                                           sizeof(params));
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: SET_PARAMS failed stream=%u capture=%u status=0x%08x buffer=%u period=%u\n",
                   StreamContext->StreamId,
                   StreamContext->Capture,
                   status,
                   params.buffer_bytes,
                   params.period_bytes);
        return status;
    }

    status = ViosndAcxPcmCommand(StreamContext->Device,
                                 StreamContext->StreamId,
                                 VIRTIO_SND_R_PCM_PREPARE);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PREPARE failed stream=%u capture=%u status=0x%08x\n",
                   StreamContext->StreamId,
                   StreamContext->Capture,
                   status);
    }
    if (NT_SUCCESS(status)) {
        StreamContext->Prepared = TRUE;
        StreamContext->PacketsCompleted = 0;
        StreamContext->NextCapturePacket = 0;
        StreamContext->PositionBytes = 0;
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream prepared stream=%u capture=%u queuedRender=%u\n",
                   StreamContext->StreamId,
                   StreamContext->Capture,
                   StreamContext->RenderReadyCount);
    }

    return status;
}

static NTSTATUS
ViosndQueueRenderPacket(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG Packet)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    NTSTATUS status = STATUS_SUCCESS;

    if (StreamContext->RenderReadyQueue == NULL ||
        StreamContext->RenderReadyCapacity == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfSpinLockAcquire(device->TxLock);
    if (StreamContext->RenderReadyCount >= StreamContext->RenderReadyCapacity) {
        StreamContext->RenderReadyHead =
            (StreamContext->RenderReadyHead + 1) % StreamContext->RenderReadyCapacity;
        StreamContext->RenderReadyCount--;
        StreamContext->XrunCount++;
    }
    StreamContext->RenderReadyQueue[StreamContext->RenderReadyTail] = Packet;
    StreamContext->RenderReadyTail =
        (StreamContext->RenderReadyTail + 1) % StreamContext->RenderReadyCapacity;
    StreamContext->RenderReadyCount++;
    WdfSpinLockRelease(device->TxLock);
    return status;
}

static BOOLEAN
ViosndPeekRenderPacket(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _Out_ PULONG Packet)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    BOOLEAN found = FALSE;

    WdfSpinLockAcquire(device->TxLock);
    if (StreamContext->RenderReadyCount != 0) {
        *Packet = StreamContext->RenderReadyQueue[StreamContext->RenderReadyHead];
        found = TRUE;
    }
    WdfSpinLockRelease(device->TxLock);
    return found;
}

static VOID
ViosndPopRenderPacket(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;

    WdfSpinLockAcquire(device->TxLock);
    if (StreamContext->RenderReadyCount != 0) {
        StreamContext->RenderReadyHead =
            (StreamContext->RenderReadyHead + 1) % StreamContext->RenderReadyCapacity;
        StreamContext->RenderReadyCount--;
    }
    WdfSpinLockRelease(device->TxLock);
}

static VOID
ViosndClearRenderQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;

    if (StreamContext->Capture) {
        return;
    }

    WdfSpinLockAcquire(device->TxLock);
    StreamContext->RenderReadyHead = 0;
    StreamContext->RenderReadyTail = 0;
    StreamContext->RenderReadyCount = 0;
    WdfSpinLockRelease(device->TxLock);
}

static NTSTATUS
ViosndSubmitRenderPacket(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG Packet)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    PVIOSND_ACX_PERIOD period;
    VIRTIO_SND_PCM_XFER *xfer;
    PUCHAR packetVa;
    VirtIOBufferDescriptor sg[2];
    NTSTATUS status = STATUS_SUCCESS;

    if (StreamContext->Capture ||
        StreamContext->Periods == NULL ||
        StreamContext->PeriodCount == 0 ||
        StreamContext->PacketCount == 0 ||
        device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    packetVa = ViosndGetPacketVa(StreamContext, Packet);
    if (packetVa == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfSpinLockAcquire(device->TxLock);
    period = NULL;
    for (ULONG i = 0; i < StreamContext->PeriodCount; ++i) {
        if (!StreamContext->Periods[i].InFlight) {
            period = &StreamContext->Periods[i];
            break;
        }
    }
    if (period == NULL) {
        WdfSpinLockRelease(device->TxLock);
        return STATUS_DEVICE_BUSY;
    }

    RtlZeroMemory(period->RequestVa, period->RequestLength);
    RtlZeroMemory(period->StatusVa, sizeof(*period->StatusVa));
    xfer = (VIRTIO_SND_PCM_XFER *)period->RequestVa;
    xfer->stream_id = StreamContext->StreamId;
        RtlCopyMemory((PUCHAR)period->RequestVa + sizeof(*xfer),
                  packetVa,
                  StreamContext->PeriodBytes);

    period->PacketNumber = Packet;
    period->Length = StreamContext->PeriodBytes;
    period->Ready = FALSE;

    sg[0].physAddr = period->RequestPa;
    sg[0].length = sizeof(*xfer) + StreamContext->PeriodBytes;
    sg[1].physAddr = period->StatusPa;
    sg[1].length = sizeof(*period->StatusVa);

    if (virtqueue_add_buf(device->Queues[VIRTIO_SND_VQ_TX],
                          sg,
                          1,
                          1,
                          period,
                          NULL,
                          0) < 0) {
        status = STATUS_DEVICE_BUSY;
    } else {
        period->InFlight = TRUE;
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: tx submit stream=%u packet=%u period=%u bytes=%u readyCount=%u\n",
                   StreamContext->StreamId,
                   Packet,
                   period->Index,
                   StreamContext->PeriodBytes,
                   StreamContext->RenderReadyCount);
        virtqueue_kick(device->Queues[VIRTIO_SND_VQ_TX]);
    }

    WdfSpinLockRelease(device->TxLock);
    return status;
}

static VOID
ViosndPumpRenderQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    ULONG packet;

    if (StreamContext == NULL ||
        StreamContext->Capture ||
        !StreamContext->Prepared) {
        return;
    }

    if (InterlockedCompareExchange(&StreamContext->RenderPumpActive, 1, 0) != 0) {
        return;
    }

    while (ViosndPeekRenderPacket(StreamContext, &packet)) {
        NTSTATUS status;

        status = ViosndSubmitRenderPacket(StreamContext, packet);
        if (status == STATUS_DEVICE_BUSY) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: tx pump busy stream=%u ready=%u\n",
                       StreamContext->StreamId,
                       StreamContext->RenderReadyCount);
            break;
        }
        if (!NT_SUCCESS(status)) {
            StreamContext->XrunCount++;
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: tx submit failed stream=%u packet=%u status=0x%08x\n",
                       StreamContext->StreamId,
                       packet,
                       status);
            ViosndPopRenderPacket(StreamContext);
            break;
        }

        ViosndPopRenderPacket(StreamContext);
    }

    InterlockedExchange(&StreamContext->RenderPumpActive, 0);
}

static NTSTATUS
ViosndSubmitCapturePeriod(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _Inout_ PVIOSND_ACX_PERIOD Period,
    _In_ ULONGLONG PacketNumber)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    VIRTIO_SND_PCM_XFER *xfer;
    VirtIOBufferDescriptor sg[3];
    NTSTATUS status = STATUS_SUCCESS;

    if (!StreamContext->Capture ||
        Period == NULL ||
        device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfSpinLockAcquire(device->RxLock);
    if (Period->InFlight) {
        WdfSpinLockRelease(device->RxLock);
        return STATUS_DEVICE_BUSY;
    }

    RtlZeroMemory(Period->RequestVa, Period->RequestLength);
    RtlZeroMemory(Period->AudioVa, StreamContext->PeriodBytes);
    RtlZeroMemory(Period->StatusVa, sizeof(*Period->StatusVa));

    xfer = (VIRTIO_SND_PCM_XFER *)Period->RequestVa;
    xfer->stream_id = StreamContext->StreamId;
    Period->PacketNumber = PacketNumber;
    Period->Length = StreamContext->PeriodBytes;

    sg[0].physAddr = Period->RequestPa;
    sg[0].length = sizeof(*xfer);
    sg[1].physAddr = Period->AudioPa;
    sg[1].length = StreamContext->PeriodBytes;
    sg[2].physAddr = Period->StatusPa;
    sg[2].length = sizeof(*Period->StatusVa);

    if (virtqueue_add_buf(device->Queues[VIRTIO_SND_VQ_RX],
                          sg,
                          1,
                          2,
                          Period,
                          NULL,
                          0) < 0) {
        status = STATUS_DEVICE_BUSY;
    } else {
        Period->InFlight = TRUE;
        virtqueue_kick(device->Queues[VIRTIO_SND_VQ_RX]);
    }

    WdfSpinLockRelease(device->RxLock);
    return status;
}

static VOID
ViosndDrainEventQueue(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    unsigned int usedLength;
    PVIOSND_EVENT_BUFFER buffer;

    if (Device->Queues[VIRTIO_SND_VQ_EVENT] == NULL) {
        return;
    }

    for (;;) {
        WdfSpinLockAcquire(Device->EventLock);
        buffer = (PVIOSND_EVENT_BUFFER)virtqueue_get_buf(Device->Queues[VIRTIO_SND_VQ_EVENT],
                                                         &usedLength);
        if (buffer != NULL) {
            if (buffer->EventVa->hdr.code == VIRTIO_SND_EVT_PCM_XRUN) {
                if (Device->RenderStream != NULL) {
                    Device->RenderStream->XrunCount++;
                }
                if (Device->CaptureStream != NULL) {
                    Device->CaptureStream->XrunCount++;
                }
            }
            (VOID)ViosndAcxPostEventBufferLocked(Device, buffer);
            virtqueue_kick(Device->Queues[VIRTIO_SND_VQ_EVENT]);
        }
        WdfSpinLockRelease(Device->EventLock);

        if (buffer == NULL) {
            break;
        }
        UNREFERENCED_PARAMETER(usedLength);
    }
}

static VOID
ViosndDrainRenderQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    unsigned int usedLength;

    if (device->Queues[VIRTIO_SND_VQ_TX] == NULL) {
        return;
    }

    for (;;) {
        PVIOSND_ACX_PERIOD period;
        LARGE_INTEGER qpc;

        WdfSpinLockAcquire(device->TxLock);
        period = (PVIOSND_ACX_PERIOD)virtqueue_get_buf(device->Queues[VIRTIO_SND_VQ_TX],
                                                       &usedLength);
        if (period != NULL) {
            period->InFlight = FALSE;
        }
        WdfSpinLockRelease(device->TxLock);

        if (period == NULL) {
            break;
        }

        if (period->StatusVa->status != VIRTIO_SND_S_OK) {
            StreamContext->XrunCount++;
        }
        StreamContext->LastLatencyBytes = period->StatusVa->latency_bytes;
        if (period->PacketNumber + 1 > StreamContext->PacketsCompleted) {
            StreamContext->PacketsCompleted = period->PacketNumber + 1;
        }
        StreamContext->PositionBytes = StreamContext->PacketsCompleted *
                                       StreamContext->PeriodBytes;
        qpc = KeQueryPerformanceCounter(NULL);
        (VOID)AcxRtStreamNotifyPacketComplete(StreamContext->Stream,
                                              period->PacketNumber,
                                              qpc.QuadPart);
        UNREFERENCED_PARAMETER(usedLength);
    }

    ViosndPumpRenderQueue(StreamContext);
}

static VOID
ViosndDrainCaptureQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    PVIOSND_ACX_DEVICE device = StreamContext->Device;
    unsigned int usedLength;

    if (device->Queues[VIRTIO_SND_VQ_RX] == NULL) {
        return;
    }

    for (;;) {
        PVIOSND_ACX_PERIOD period;
        PUCHAR packetVa;
        LARGE_INTEGER qpc;

        WdfSpinLockAcquire(device->RxLock);
        period = (PVIOSND_ACX_PERIOD)virtqueue_get_buf(device->Queues[VIRTIO_SND_VQ_RX],
                                                       &usedLength);
        if (period != NULL) {
            period->InFlight = FALSE;
        }
        WdfSpinLockRelease(device->RxLock);

        if (period == NULL) {
            break;
        }

        packetVa = ViosndGetPacketVa(StreamContext, (ULONG)period->PacketNumber);
        if (packetVa != NULL) {
            RtlCopyMemory(packetVa, period->AudioVa, StreamContext->PeriodBytes);
        }

        if (period->StatusVa->status != VIRTIO_SND_S_OK) {
            StreamContext->XrunCount++;
        }
        StreamContext->LastLatencyBytes = period->StatusVa->latency_bytes;
        if (period->PacketNumber + 1 > StreamContext->PacketsCompleted) {
            StreamContext->PacketsCompleted = period->PacketNumber + 1;
        }
        StreamContext->PositionBytes = StreamContext->PacketsCompleted *
                                       StreamContext->PeriodBytes;
        qpc = KeQueryPerformanceCounter(NULL);
        (VOID)AcxRtStreamNotifyPacketComplete(StreamContext->Stream,
                                              period->PacketNumber,
                                              qpc.QuadPart);

        if (StreamContext->Running) {
            NTSTATUS status;
            status = ViosndSubmitCapturePeriod(StreamContext,
                                               period,
                                               StreamContext->NextCapturePacket);
            if (NT_SUCCESS(status)) {
                StreamContext->NextCapturePacket++;
            } else if (status != STATUS_DEVICE_BUSY) {
                StreamContext->XrunCount++;
            }
        }
        UNREFERENCED_PARAMETER(usedLength);
    }
}

static VOID
ViosndDrainStreamQueue(
    _Inout_ PVIOSND_ACX_STREAM StreamContext)
{
    if (StreamContext == NULL) {
        return;
    }

    if (StreamContext->Capture) {
        ViosndDrainCaptureQueue(StreamContext);
    } else {
        ViosndDrainRenderQueue(StreamContext);
    }
}

static BOOLEAN
ViosndStreamHasInFlight(
    _In_ PVIOSND_ACX_STREAM StreamContext)
{
    PVIOSND_ACX_DEVICE device;
    WDFSPINLOCK lock;
    BOOLEAN inFlight = FALSE;

    if (StreamContext == NULL || StreamContext->Periods == NULL) {
        return FALSE;
    }

    device = StreamContext->Device;
    lock = StreamContext->Capture ? device->RxLock : device->TxLock;
    WdfSpinLockAcquire(lock);
    for (ULONG i = 0; i < StreamContext->PeriodCount; ++i) {
        if (StreamContext->Periods[i].InFlight) {
            inFlight = TRUE;
            break;
        }
    }
    WdfSpinLockRelease(lock);
    return inFlight;
}

static VOID
ViosndWaitForStreamIdle(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ ULONG TimeoutMs)
{
    ULONG elapsedMs = 0;

    for (;;) {
        ViosndDrainStreamQueue(StreamContext);
        if (!ViosndStreamHasInFlight(StreamContext)) {
            break;
        }
        if (elapsedMs >= TimeoutMs) {
            StreamContext->XrunCount++;
            break;
        }

        LARGE_INTEGER interval;
        interval.QuadPart = -10LL * 1000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        ++elapsedMs;
    }
}

static VOID
ViosndStopStream(
    _Inout_ PVIOSND_ACX_STREAM StreamContext,
    _In_ BOOLEAN Release)
{
    if (StreamContext == NULL) {
        return;
    }

    StreamContext->Running = FALSE;
    if (!StreamContext->Prepared) {
        return;
    }

    (VOID)ViosndAcxPcmCommand(StreamContext->Device,
                              StreamContext->StreamId,
                              VIRTIO_SND_R_PCM_STOP);
    ViosndClearRenderQueue(StreamContext);

    if (Release) {
        (VOID)ViosndAcxPcmCommand(StreamContext->Device,
                                  StreamContext->StreamId,
                                  VIRTIO_SND_R_PCM_RELEASE);
        ViosndWaitForStreamIdle(StreamContext, 1000);
        StreamContext->Prepared = FALSE;
    } else {
        ViosndWaitForStreamIdle(StreamContext, 20);
    }
}

VOID
ViosndAcxStopStreams(
    _Inout_ PVIOSND_ACX_DEVICE Device,
    _In_ BOOLEAN Release)
{
    ViosndStopStream(Device->RenderStream, Release);
    ViosndStopStream(Device->CaptureStream, Release);
    ViosndAcxDrainQueues(Device);
}

VOID
ViosndAcxDrainQueues(
    _Inout_ PVIOSND_ACX_DEVICE Device)
{
    ViosndDrainEventQueue(Device);
    ViosndDrainStreamQueue(Device->RenderStream);
    ViosndDrainStreamQueue(Device->CaptureStream);
}

static NTSTATUS
ViosndEvtCircuitCreateStream(
    _In_ WDFDEVICE Device,
    _In_ ACXCIRCUIT Circuit,
    _In_ ACXPIN Pin,
    _In_ PACXSTREAM_INIT StreamInit,
    _In_ ACXDATAFORMAT StreamFormat,
    _In_ const GUID *SignalProcessingMode,
    _In_ ACXOBJECTBAG VarArguments)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_STREAM_CALLBACKS streamCallbacks;
    ACX_RT_STREAM_CALLBACKS rtCallbacks;
    ACXSTREAM stream;
    PVIOSND_ACX_DEVICE deviceContext = ViosndGetDeviceContext(Device);
    PVIOSND_ACX_STREAM streamContext;
    ULONG pinId = AcxPinGetId(Pin);
    PVIOSND_ACX_PIN_CONTEXT pinContext;
    BOOLEAN capture = FALSE;
    ULONG streamId = MAXULONG;

    UNREFERENCED_PARAMETER(VarArguments);
    pinContext = ViosndGetPinContext(Pin);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX create stream request pin=%u pinType=%u pinRole=%u circuit=%p renderCircuit=%p captureCircuit=%p fmt=%uHz/%ub/%uch mode=%p\n",
               pinId,
               AcxPinGetType(Pin),
               pinContext != NULL ? pinContext->Role : MAXULONG,
               Circuit,
               deviceContext->RenderCircuit,
               deviceContext->CaptureCircuit,
               StreamFormat != NULL ? AcxDataFormatGetSampleRate(StreamFormat) : 0,
               StreamFormat != NULL ? AcxDataFormatGetBitsPerSample(StreamFormat) : 0,
               StreamFormat != NULL ? AcxDataFormatGetChannelsCount(StreamFormat) : 0,
               SignalProcessingMode);

    if (pinContext == NULL || pinContext->Role != ViosndAcxPinRoleHost) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX create stream rejected pin=%u role=%u\n",
                   pinId,
                   pinContext != NULL ? pinContext->Role : MAXULONG);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Circuit == deviceContext->CaptureCircuit) {
        capture = TRUE;
        streamId = deviceContext->Streams.CaptureStreamId;
    } else if (Circuit == deviceContext->RenderCircuit) {
        capture = FALSE;
        streamId = deviceContext->Streams.RenderStreamId;
    } else {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
    streamCallbacks.EvtAcxStreamPrepareHardware = ViosndEvtStreamPrepareHardware;
    streamCallbacks.EvtAcxStreamReleaseHardware = ViosndEvtStreamReleaseHardware;
    streamCallbacks.EvtAcxStreamRun = ViosndEvtStreamRun;
    streamCallbacks.EvtAcxStreamPause = ViosndEvtStreamPause;
    status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &streamCallbacks);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateStream.AssignCallbacks", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX assign stream callbacks failed stream=%u capture=%u status=0x%08x\n",
                   streamId,
                   capture,
                   status);
        return status;
    }
    ViosndAcxWriteDiag(Device, L"CreateStream.AssignCallbacks", STATUS_SUCCESS);

    ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
    rtCallbacks.EvtAcxStreamGetHwLatency = ViosndEvtStreamGetHwLatency;
    rtCallbacks.EvtAcxStreamAllocateRtPackets = ViosndEvtStreamAllocateRtPackets;
    rtCallbacks.EvtAcxStreamFreeRtPackets = ViosndEvtStreamFreeRtPackets;
    rtCallbacks.EvtAcxStreamGetCurrentPacket = ViosndEvtStreamGetCurrentPacket;
    rtCallbacks.EvtAcxStreamGetPresentationPosition =
        ViosndEvtStreamGetPresentationPosition;
    if (capture) {
        rtCallbacks.EvtAcxStreamGetCapturePacket = ViosndEvtStreamGetCapturePacket;
    } else {
        rtCallbacks.EvtAcxStreamSetRenderPacket = ViosndEvtStreamSetRenderPacket;
    }
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rtCallbacks);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"CreateStream.AssignRtCallbacks", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX assign rt callbacks failed stream=%u capture=%u status=0x%08x\n",
                   streamId,
                   capture,
                   status);
        return status;
    }
    ViosndAcxWriteDiag(Device, L"CreateStream.AssignRtCallbacks", STATUS_SUCCESS);
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VIOSND_ACX_STREAM);
    attributes.EvtCleanupCallback = ViosndEvtStreamCleanup;
    status = AcxRtStreamCreate(Device, Circuit, &attributes, &StreamInit, &stream);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX stream create failed streamId=%u capture=%u status=0x%08x\n",
                   streamId,
                   capture,
                   status);
        return status;
    }

    streamContext = ViosndGetStreamContext(stream);
    RtlZeroMemory(streamContext, sizeof(*streamContext));
    streamContext->Device = deviceContext;
    streamContext->Stream = stream;
    streamContext->StreamId = streamId;
    streamContext->Capture = capture;
    if (capture) {
        deviceContext->CaptureStream = streamContext;
    } else {
        deviceContext->RenderStream = streamContext;
    }
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream created stream=%u capture=%u\n",
               streamContext->StreamId,
               streamContext->Capture);
    return STATUS_SUCCESS;
}

static VOID
ViosndEvtStreamCleanup(
    _In_ WDFOBJECT Object)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Object);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream cleanup stream=%u capture=%u prepared=%u running=%u packets=%u completed=%llu\n",
               context->StreamId,
               context->Capture,
               context->Prepared,
               context->Running,
               context->PacketCount,
               context->PacketsCompleted);

    if (context->Device != NULL) {
        if (context->Capture && context->Device->CaptureStream == context) {
            context->Device->CaptureStream = NULL;
        } else if (!context->Capture && context->Device->RenderStream == context) {
            context->Device->RenderStream = NULL;
        }
    }

    ViosndFreeStreamPeriods(context);
}

static NTSTATUS
ViosndEvtStreamPrepareHardware(
    _In_ ACXSTREAM Stream)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    NTSTATUS status;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream prepare stream=%u capture=%u\n",
               context->StreamId,
               context->Capture);
    status = ViosndConfigurePcmStream(context);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX stream prepare failed stream=%u capture=%u status=0x%08x\n",
                   context->StreamId,
                   context->Capture,
                   status);
        ViosndFreeStreamPeriods(context);
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX stream prepare done stream=%u capture=%u packets=%u packetSize=%u periods=%u periodBytes=%u\n",
                   context->StreamId,
                   context->Capture,
                   context->PacketCount,
                   context->PacketSize,
                   context->PeriodCount,
                   context->PeriodBytes);
    }
    return status;
}

static NTSTATUS
ViosndEvtStreamReleaseHardware(
    _In_ ACXSTREAM Stream)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    PVIOSND_ACX_DEVICE device = context->Device;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream release stream=%u capture=%u\n",
               context->StreamId,
               context->Capture);

    ViosndStopStream(context, TRUE);
    ViosndAcxDrainQueues(device);
    ViosndFreeStreamPeriods(context);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamRun(
    _In_ ACXSTREAM Stream)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    NTSTATUS status;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream run stream=%u capture=%u\n",
               context->StreamId,
               context->Capture);

    if (context->Running) {
        return STATUS_SUCCESS;
    }

    if (!context->Prepared) {
        status = ViosndConfigurePcmStream(context);
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: ACX stream run configure failed stream=%u capture=%u status=0x%08x\n",
                       context->StreamId,
                       context->Capture,
                       status);
            return status;
        }
    }

    if (context->Capture) {
        for (ULONG i = 0; i < context->PeriodCount; ++i) {
            status = ViosndSubmitCapturePeriod(context,
                                               &context->Periods[i],
                                               context->NextCapturePacket);
            if (status == STATUS_DEVICE_BUSY) {
                continue;
            }
            if (!NT_SUCCESS(status)) {
                return status;
            }
            context->NextCapturePacket++;
        }
    } else {
        ViosndPumpRenderQueue(context);
    }

    status = ViosndAcxPcmCommand(context->Device,
                                 context->StreamId,
                                 VIRTIO_SND_R_PCM_START);
    if (!NT_SUCCESS(status)) {
        context->Running = FALSE;
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: START failed stream=%u capture=%u status=0x%08x\n",
                   context->StreamId,
                   context->Capture,
                   status);
        return status;
    }

    context->Running = TRUE;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: START done stream=%u capture=%u ready=%u periods=%u\n",
               context->StreamId,
               context->Capture,
               context->RenderReadyCount,
               context->PeriodCount);
    if (!context->Capture) {
        ViosndPumpRenderQueue(context);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamPause(
    _In_ ACXSTREAM Stream)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX stream pause stream=%u capture=%u\n",
               context->StreamId,
               context->Capture);
    ViosndStopStream(context, FALSE);
    ViosndAcxDrainQueues(context->Device);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamGetHwLatency(
    _In_ ACXSTREAM Stream,
    _Out_ ULONG *FifoSize,
    _Out_ ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    *FifoSize = VIOSND_DEFAULT_PERIOD_BYTES;
    *Delay = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamAllocateRtPackets(
    _In_ ACXSTREAM Stream,
    _In_ ULONG PacketCount,
    _In_ ULONG PacketSize,
    _Out_ PACX_RTPACKET *Packets)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    ULONG allocBytes;
    ULONG firstPacketOffset;

    *Packets = NULL;
    if (PacketCount == 0 || PacketSize == 0 || PacketSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    ViosndFreeRtPacketStorage(context);

    allocBytes = (PacketSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    firstPacketOffset = allocBytes - PacketSize;
    context->Packets = (PACX_RTPACKET)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                  PacketCount * sizeof(ACX_RTPACKET),
                                                                  VIOSND_POOL_TAG);
    context->PacketBuffers = (PVOID *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                  PacketCount * sizeof(PVOID),
                                                                  VIOSND_POOL_TAG);
    context->PacketMdls = (PMDL *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                             PacketCount * sizeof(PMDL),
                                                             VIOSND_POOL_TAG);
    if (!context->Capture) {
        context->RenderReadyQueue =
            (ULONG *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                 PacketCount * sizeof(ULONG),
                                                 VIOSND_POOL_TAG);
    }
    if (context->Packets == NULL ||
        context->PacketBuffers == NULL ||
        context->PacketMdls == NULL) {
        ViosndEvtStreamFreeRtPackets(Stream, context->Packets, PacketCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!context->Capture && context->RenderReadyQueue == NULL) {
        ViosndEvtStreamFreeRtPackets(Stream, context->Packets, PacketCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(context->Packets, PacketCount * sizeof(ACX_RTPACKET));
    RtlZeroMemory(context->PacketBuffers, PacketCount * sizeof(PVOID));
    RtlZeroMemory(context->PacketMdls, PacketCount * sizeof(PMDL));
    if (context->RenderReadyQueue != NULL) {
        RtlZeroMemory(context->RenderReadyQueue, PacketCount * sizeof(ULONG));
    }
    context->PacketCount = PacketCount;
    for (ULONG i = 0; i < PacketCount; ++i) {
        context->PacketBuffers[i] = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                allocBytes,
                                                                VIOSND_POOL_TAG);
        if (context->PacketBuffers[i] == NULL) {
            ViosndEvtStreamFreeRtPackets(Stream, context->Packets, PacketCount);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(context->PacketBuffers[i], allocBytes);

        context->PacketMdls[i] = IoAllocateMdl(context->PacketBuffers[i],
                                               allocBytes,
                                               FALSE,
                                               FALSE,
                                               NULL);
        if (context->PacketMdls[i] == NULL) {
            ViosndEvtStreamFreeRtPackets(Stream, context->Packets, PacketCount);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(context->PacketMdls[i]);

        ACX_RTPACKET_INIT(&context->Packets[i]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&context->Packets[i].RtPacketBuffer,
                                       context->PacketMdls[i],
                                       allocBytes);
        context->Packets[i].RtPacketOffset = (i == 0) ? firstPacketOffset : 0;
        context->Packets[i].RtPacketSize = PacketSize;
    }

    context->PacketCount = PacketCount;
    context->PacketSize = PacketSize;
    context->PacketAllocBytes = allocBytes;
    context->FirstPacketOffset = firstPacketOffset;
    context->RenderReadyCapacity = context->Capture ? 0 : PacketCount;
    context->RenderReadyHead = 0;
    context->RenderReadyTail = 0;
    context->RenderReadyCount = 0;
    *Packets = context->Packets;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX allocated rt packets stream=%u capture=%u count=%u size=%u alloc=%u firstOffset=%u\n",
               context->StreamId,
               context->Capture,
               PacketCount,
               PacketSize,
               allocBytes,
               firstPacketOffset);
    return STATUS_SUCCESS;
}

static VOID
ViosndEvtStreamFreeRtPackets(
    _In_ ACXSTREAM Stream,
    _In_ PACX_RTPACKET Packets,
    _In_ ULONG PacketCount)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);

    UNREFERENCED_PARAMETER(Packets);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: ACX free rt packets stream=%u capture=%u count=%u stored=%u completed=%llu\n",
               context->StreamId,
               context->Capture,
               PacketCount,
               context->PacketCount,
               context->PacketsCompleted);
    ViosndFreeRtPacketStorage(context);
}

static NTSTATUS
ViosndEvtStreamSetRenderPacket(
    _In_ ACXSTREAM Stream,
    _In_ ULONG Packet,
    _In_ ULONG Flags,
    _In_ ULONG EosPacketLength)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);

    if (context->Capture) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (context->RenderReadyQueue == NULL || context->RenderReadyCapacity == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: set render packet before rt allocation stream=%u packet=%u\n",
                   context->StreamId,
                   Packet);
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status;
    status = ViosndQueueRenderPacket(context, Packet);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (!context->Running) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: render packet queued before run stream=%u packet=%u ready=%u prepared=%u\n",
                   context->StreamId,
                   Packet,
                   context->RenderReadyCount,
                   context->Prepared);
        return STATUS_SUCCESS;
    }

    ViosndPumpRenderQueue(context);
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamGetCurrentPacket(
    _In_ ACXSTREAM Stream,
    _Out_ PULONG CurrentPacket)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    *CurrentPacket = (ULONG)context->PacketsCompleted;
    if (context->CurrentPacketQueryCount < 8 ||
        (context->CurrentPacketQueryCount % 128) == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX get current packet stream=%u capture=%u current=%u query=%u prepared=%u running=%u\n",
                   context->StreamId,
                   context->Capture,
                   *CurrentPacket,
                   context->CurrentPacketQueryCount,
                   context->Prepared,
                   context->Running);
    }
    context->CurrentPacketQueryCount++;
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamGetCapturePacket(
    _In_ ACXSTREAM Stream,
    _Out_ PULONG LastCapturePacket,
    _Out_ PULONGLONG QPCPacketStart,
    _Out_ PBOOLEAN MoreData)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);

    if (!context->Capture) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (context->PacketsCompleted == 0) {
        *LastCapturePacket = 0;
        *QPCPacketStart = KeQueryPerformanceCounter(NULL).QuadPart;
        *MoreData = FALSE;
        return STATUS_DEVICE_NOT_READY;
    }

    *LastCapturePacket = (ULONG)(context->PacketsCompleted - 1);
    *QPCPacketStart = KeQueryPerformanceCounter(NULL).QuadPart;
    *MoreData = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS
ViosndEvtStreamGetPresentationPosition(
    _In_ ACXSTREAM Stream,
    _Out_ PULONGLONG PositionInBlocks,
    _Out_ PULONGLONG QPCPosition)
{
    PVIOSND_ACX_STREAM context = ViosndGetStreamContext(Stream);
    ULONG blockAlign = (VIOSND_DEFAULT_CHANNELS * VIOSND_DEFAULT_BITS_PER_SAMPLE) / 8;

    *PositionInBlocks = blockAlign != 0 ? context->PositionBytes / blockAlign : 0;
    *QPCPosition = KeQueryPerformanceCounter(NULL).QuadPart;
    if (context->PresentationQueryCount < 8 ||
        (context->PresentationQueryCount % 128) == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ACX get presentation stream=%u capture=%u blocks=%llu bytes=%llu query=%u prepared=%u running=%u\n",
                   context->StreamId,
                   context->Capture,
                   *PositionInBlocks,
                   context->PositionBytes,
                   context->PresentationQueryCount,
                   context->Prepared,
                   context->Running);
    }
    context->PresentationQueryCount++;
    return STATUS_SUCCESS;
}
