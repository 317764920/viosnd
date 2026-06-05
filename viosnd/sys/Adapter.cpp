#include "precomp.h"

static PVIOSND_DEVICE g_ViosndDevice;

#define VIOSND_ENDPOINT_ROLE_RENDER  0u
#define VIOSND_ENDPOINT_ROLE_CAPTURE 1u
#define VIOSND_ENDPOINT_ROLE_BOTH    2u

typedef struct _VIOSND_SUBDEVICE {
    PPORT Port;
    PMINIPORT Miniport;
} VIOSND_SUBDEVICE, *PVIOSND_SUBDEVICE;

static
VOID
ViosndReleaseSubdevice(
    _Inout_ PVIOSND_SUBDEVICE Subdevice)
{
    if (Subdevice->Miniport != NULL) {
        Subdevice->Miniport->Release();
        Subdevice->Miniport = NULL;
    }
    if (Subdevice->Port != NULL) {
        Subdevice->Port->Release();
        Subdevice->Port = NULL;
    }
}

static
ULONG
ViosndReadDeviceDword(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR ValueName,
    _In_ ULONG DefaultValue)
{
    HANDLE key = NULL;
    NTSTATUS status;
    UNICODE_STRING valueName;
    ULONG resultLength;
    ULONG value = DefaultValue;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION information;

    status = IoOpenDeviceRegistryKey(PhysicalDeviceObject,
                                     PLUGPLAY_REGKEY_DEVICE,
                                     KEY_READ,
                                     &key);
    if (!NT_SUCCESS(status)) {
        return DefaultValue;
    }

    RtlInitUnicodeString(&valueName, ValueName);
    information = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    RtlZeroMemory(buffer, sizeof(buffer));

    status = ZwQueryValueKey(key,
                             &valueName,
                             KeyValuePartialInformation,
                             information,
                             sizeof(buffer),
                             &resultLength);
    if (NT_SUCCESS(status) &&
        information->Type == REG_DWORD &&
        information->DataLength >= sizeof(ULONG)) {
        value = *((PULONG)information->Data);
    }

    ZwClose(key);
    return value;
}

static
NTSTATUS
ViosndCreateAndRegisterWaveRTSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture,
    _In_ PWSTR Name,
    _Out_ PVIOSND_SUBDEVICE Subdevice)
{
    NTSTATUS status;

    RtlZeroMemory(Subdevice, sizeof(*Subdevice));

    status = PcNewPort(&Subdevice->Port, CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcNewPort(WaveRT %ws) failed 0x%08x\n",
                   Name,
                   status);
        return status;
    }

    status = ViosndCreateWaveRTMiniport(Device, StreamId, Capture, &Subdevice->Miniport);
    if (NT_SUCCESS(status)) {
        status = Subdevice->Port->Init(DeviceObject,
                                       Irp,
                                       Subdevice->Miniport,
                                       NULL,
                                       ResourceList);
    }
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: WaveRT Init %ws failed 0x%08x\n",
                   Name,
                   status);
    }

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, Name, Subdevice->Port);
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterSubdevice(WaveRT %ws) failed 0x%08x\n",
                       Name,
                       status);
        }
    }

    if (!NT_SUCCESS(status)) {
        ViosndReleaseSubdevice(Subdevice);
    }
    return status;
}

static
NTSTATUS
ViosndCreateAndRegisterTopologySubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ BOOLEAN Capture,
    _In_ PWSTR Name,
    _Out_ PVIOSND_SUBDEVICE Subdevice)
{
    NTSTATUS status;

    RtlZeroMemory(Subdevice, sizeof(*Subdevice));

    status = PcNewPort(&Subdevice->Port, CLSID_PortTopology);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcNewPort(Topology %ws) failed 0x%08x\n",
                   Name,
                   status);
        return status;
    }

    status = ViosndCreateTopologyMiniport(Capture, &Subdevice->Miniport);
    if (NT_SUCCESS(status)) {
        status = Subdevice->Port->Init(DeviceObject,
                                       Irp,
                                       Subdevice->Miniport,
                                       NULL,
                                       ResourceList);
    }
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: Topology Init %ws failed 0x%08x\n",
                   Name,
                   status);
    }

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, Name, Subdevice->Port);
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterSubdevice(Topology %ws) failed 0x%08x\n",
                       Name,
                       status);
        }
    }

    if (!NT_SUCCESS(status)) {
        ViosndReleaseSubdevice(Subdevice);
    }
    return status;
}

static
NTSTATUS
ViosndRegisterAudioEndpoint(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture,
    _In_ PWSTR TopologyName,
    _In_ PWSTR WaveName)
{
    NTSTATUS status;
    VIOSND_SUBDEVICE topology;
    VIOSND_SUBDEVICE wave;

    RtlZeroMemory(&topology, sizeof(topology));
    RtlZeroMemory(&wave, sizeof(wave));

    status = ViosndCreateAndRegisterTopologySubdevice(DeviceObject,
                                                      Irp,
                                                      ResourceList,
                                                      Capture,
                                                      TopologyName,
                                                      &topology);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ViosndCreateAndRegisterWaveRTSubdevice(DeviceObject,
                                                    Irp,
                                                    ResourceList,
                                                    Device,
                                                    StreamId,
                                                    Capture,
                                                    WaveName,
                                                    &wave);
    if (NT_SUCCESS(status)) {
        if (Capture) {
            status = PcRegisterPhysicalConnection(DeviceObject,
                                                  topology.Port,
                                                  VIOSND_TOPO_PIN_BRIDGE,
                                                  wave.Port,
                                                  VIOSND_PIN_BRIDGE);
        } else {
            status = PcRegisterPhysicalConnection(DeviceObject,
                                                  wave.Port,
                                                  VIOSND_PIN_BRIDGE,
                                                  topology.Port,
                                                  VIOSND_TOPO_PIN_SOURCE);
        }
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterPhysicalConnection %ws/%ws failed 0x%08x\n",
                       WaveName,
                       TopologyName,
                       status);
        }
    }

    ViosndReleaseSubdevice(&wave);
    ViosndReleaseSubdevice(&topology);
    return status;
}

extern "C"
NTSTATUS
XcbVirtioAudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    NTSTATUS status;
    PVIOSND_DEVICE device;
    PDEVICE_OBJECT physicalDeviceObject;
    VIOSND_STREAM_PAIR streams;
    ULONG endpointRole;
    BOOLEAN enableRender;
    BOOLEAN enableCapture;

    status = PcGetPhysicalDeviceObject(DeviceObject, &physicalDeviceObject);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcGetPhysicalDeviceObject failed 0x%08x\n",
                   status);
        return status;
    }

    endpointRole = ViosndReadDeviceDword(physicalDeviceObject,
                                         L"EndpointRole",
                                         VIOSND_ENDPOINT_ROLE_BOTH);
    enableRender = endpointRole != VIOSND_ENDPOINT_ROLE_CAPTURE;
    enableCapture = endpointRole != VIOSND_ENDPOINT_ROLE_RENDER;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: endpoint role=%u render=%u capture=%u\n",
               endpointRole,
               enableRender,
               enableCapture);

    status = ViosndCreateDevice(DeviceObject, physicalDeviceObject, ResourceList, &device);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ViosndCreateDevice failed 0x%08x\n",
                   status);
        return status;
    }

    status = ViosndInitializeDevice(device);
    if (NT_SUCCESS(status)) {
        status = ViosndQueryPcmStreams(device, &streams);
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: init/query streams failed 0x%08x\n",
                   status);
        ViosndDestroyDevice(device);
        return status;
    }

    if (enableRender && streams.HasRender) {
        status = ViosndConfigureDefaultPcm(device, streams.RenderStreamId);
    }

    if (NT_SUCCESS(status) && enableCapture && streams.HasCapture) {
        status = ViosndConfigureDefaultPcm(device, streams.CaptureStreamId);
    }

    if (NT_SUCCESS(status) && enableRender && !streams.HasRender) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(status) && enableCapture && !streams.HasCapture) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: configure PCM failed 0x%08x render=%u/%u capture=%u/%u\n",
                   status,
                   streams.HasRender,
                   streams.RenderStreamId,
                   streams.HasCapture,
                   streams.CaptureStreamId);
        ViosndDestroyDevice(device);
        return status;
    }

    if (enableRender && streams.HasRender) {
        status = ViosndRegisterAudioEndpoint(DeviceObject,
                                             Irp,
                                             ResourceList,
                                             device,
                                             streams.RenderStreamId,
                                             FALSE,
                                             VIOSND_TOPOOUT_NAME,
                                             VIOSND_WAVEOUT_NAME);
    }

    if (NT_SUCCESS(status) && enableCapture && streams.HasCapture) {
        status = ViosndRegisterAudioEndpoint(DeviceObject,
                                             Irp,
                                             ResourceList,
                                             device,
                                             streams.CaptureStreamId,
                                             TRUE,
                                             VIOSND_TOPOIN_NAME,
                                             VIOSND_WAVEIN_NAME);
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: register audio endpoint failed 0x%08x\n",
                   status);
        ViosndDestroyDevice(device);
        return status;
    }

    g_ViosndDevice = device;
    return status;
}
