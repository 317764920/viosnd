#include "precomp.h"

PDEVICE_OBJECT g_XcbVirtioAudioPdo;

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
    if (g_XcbVirtioAudioPdo == NULL) {
        ObReferenceObject(PhysicalDeviceObject);
        g_XcbVirtioAudioPdo = PhysicalDeviceObject;
    }

    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              XcbVirtioAudioStartDevice,
                              VIOSND_MAX_SUBDEVICES,
                              0);
}
