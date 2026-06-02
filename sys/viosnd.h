#pragma once

#define VIOSND_POOL_TAG 'dnSV'
#define VIOSND_MAX_SUBDEVICES 4

extern PDEVICE_OBJECT g_XcbVirtioAudioPdo;

extern "C" DRIVER_ADD_DEVICE XcbVirtioAudioAddDevice;

extern "C" NTSTATUS
XcbVirtioAudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);
