#include "precomp.h"

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    status = PcInitializeAdapterDriver(DriverObject,
                                       RegistryPath,
                                       XcbVirtioAudioAddDevice);

    return status;
}

extern "C"
NTSTATUS
XcbVirtioAudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              XcbVirtioAudioStartDevice,
                              VIOSND_MAX_SUBDEVICES,
                              0);
}
