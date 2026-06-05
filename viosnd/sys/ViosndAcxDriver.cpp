#include "ViosndAcx.h"

BOOLEAN g_ViosndLogEnabled = TRUE;
static WCHAR g_ViosndParametersPathBuffer[512];
static UNICODE_STRING g_ViosndParametersPath;

static NTSTATUS
ViosndInitServiceParametersPath(
    _In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR suffix[] = L"\\Parameters";
    USHORT suffixBytes = sizeof(suffix) - sizeof(WCHAR);
    USHORT totalBytes;

    RtlInitUnicodeString(&g_ViosndParametersPath, NULL);

    if (RegistryPath == NULL ||
        RegistryPath->Buffer == NULL ||
        RegistryPath->Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    totalBytes = RegistryPath->Length + suffixBytes;
    if (totalBytes + sizeof(WCHAR) > sizeof(g_ViosndParametersPathBuffer)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(g_ViosndParametersPathBuffer,
                  RegistryPath->Buffer,
                  RegistryPath->Length);
    RtlCopyMemory((PUCHAR)g_ViosndParametersPathBuffer + RegistryPath->Length,
                  suffix,
                  suffixBytes);
    g_ViosndParametersPathBuffer[totalBytes / sizeof(WCHAR)] = UNICODE_NULL;

    g_ViosndParametersPath.Buffer = g_ViosndParametersPathBuffer;
    g_ViosndParametersPath.Length = totalBytes;
    g_ViosndParametersPath.MaximumLength = totalBytes + sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static VOID
ViosndWriteServiceDiag(
    _In_z_ PCWSTR Stage,
    _In_ NTSTATUS Status)
{
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING valueName;
    NTSTATUS localStatus;
    ULONG disposition;

    if (g_ViosndParametersPath.Buffer == NULL) {
        return;
    }

    InitializeObjectAttributes(&attributes,
                               &g_ViosndParametersPath,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    localStatus = ZwCreateKey(&key,
                              KEY_SET_VALUE,
                              &attributes,
                              0,
                              NULL,
                              REG_OPTION_NON_VOLATILE,
                              &disposition);
    if (!NT_SUCCESS(localStatus)) {
        return;
    }

    RtlInitUnicodeString(&valueName, L"LastServiceStage");
    (VOID)ZwSetValueKey(key,
                        &valueName,
                        0,
                        REG_SZ,
                        (PVOID)Stage,
                        ((ULONG)wcslen(Stage) + 1) * sizeof(WCHAR));

    RtlInitUnicodeString(&valueName, L"LastServiceStatus");
    (VOID)ZwSetValueKey(key,
                        &valueName,
                        0,
                        REG_DWORD,
                        &Status,
                        sizeof(Status));

    ZwClose(key);
}

VOID
ViosndLogAlways(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    va_list args;
    CHAR buffer[512];

    if (!g_ViosndLogEnabled) {
        return;
    }

    va_start(args, Format);
    if (NT_SUCCESS(RtlStringCbVPrintfA(buffer, sizeof(buffer), Format, args))) {
        DbgPrint("viosnd: %s", buffer);
    }
    va_end(args);
}

static VOID
ViosndAcxLoadLogSwitch(
    _In_ WDFDEVICE Device)
{
    WDFKEY key;
    UNICODE_STRING valueName;
    ULONG enabled;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device,
                                             PLUGPLAY_REGKEY_DEVICE,
                                             KEY_QUERY_VALUE,
                                             WDF_NO_OBJECT_ATTRIBUTES,
                                             &key))) {
        return;
    }

    RtlInitUnicodeString(&valueName, L"LogEnabled");
    if (NT_SUCCESS(WdfRegistryQueryULong(key, &valueName, &enabled))) {
        g_ViosndLogEnabled = (enabled != 0);
    }

    WdfRegistryClose(key);
}

static VOID
ViosndAcxRemoveStaticCircuits(
    _In_ WDFDEVICE Device,
    _Inout_ PVIOSND_ACX_DEVICE Context)
{
    NTSTATUS status;

    if (Context->CaptureCircuitAdded && Context->CaptureCircuit != NULL) {
        status = AcxDeviceRemoveCircuit(Device, Context->CaptureCircuit);
        ViosndAcxWriteDiag(Device, L"RemoveStaticCircuits.Capture", status);
        if (NT_SUCCESS(status)) {
            Context->CaptureCircuitAdded = FALSE;
        }
    }

    if (Context->RenderCircuitAdded && Context->RenderCircuit != NULL) {
        status = AcxDeviceRemoveCircuit(Device, Context->RenderCircuit);
        ViosndAcxWriteDiag(Device, L"RemoveStaticCircuits.Render", status);
        if (NT_SUCCESS(status)) {
            Context->RenderCircuitAdded = FALSE;
        }
    }
}

static NTSTATUS
ViosndAcxAddStaticCircuits(
    _In_ WDFDEVICE Device,
    _Inout_ PVIOSND_ACX_DEVICE Context)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Context->Streams.HasRender && !Context->RenderCircuitAdded) {
        if (Context->RenderCircuit == NULL) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            status = AcxDeviceAddCircuit(Device, Context->RenderCircuit);
        }

        ViosndAcxWriteDiag(Device, L"PrepareHardware.AddRenderCircuit", status);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Context->RenderCircuitAdded = TRUE;
    }

    if (Context->Streams.HasCapture && !Context->CaptureCircuitAdded) {
        if (Context->CaptureCircuit == NULL) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            status = AcxDeviceAddCircuit(Device, Context->CaptureCircuit);
        }

        ViosndAcxWriteDiag(Device, L"PrepareHardware.AddCaptureCircuit", status);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Context->CaptureCircuitAdded = TRUE;
    }

    return status;
}

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG wdfConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDRIVER driver;
    ACX_DRIVER_CONFIG acxConfig;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    (VOID)ViosndInitServiceParametersPath(RegistryPath);
    ViosndWriteServiceDiag(L"DriverEntry.Start", STATUS_SUCCESS);
    ViosndLogAlways("DriverEntry start\n");

    WDF_DRIVER_CONFIG_INIT(&wdfConfig, ViosndEvtDeviceAdd);
    wdfConfig.DriverPoolTag = VIOSND_POOL_TAG;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = ViosndEvtDriverContextCleanup;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &wdfConfig,
                             &driver);
    if (!NT_SUCCESS(status)) {
        ViosndWriteServiceDiag(L"DriverEntry.WdfDriverCreate", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "WdfDriverCreate failed 0x%08x\n",
                   status);
        return status;
    }

    ACX_DRIVER_CONFIG_INIT(&acxConfig);
    status = AcxDriverInitialize(driver, &acxConfig);
    if (!NT_SUCCESS(status)) {
        ViosndWriteServiceDiag(L"DriverEntry.AcxDriverInitialize", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "AcxDriverInitialize failed 0x%08x\n",
                   status);
    }

    ViosndWriteServiceDiag(L"DriverEntry.Done", status);
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "DriverEntry done 0x%08x\n",
               status);
    return status;
}

VOID
ViosndEvtDriverContextCleanup(
    _In_ WDFOBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

VOID
ViosndAcxWriteDiag(
    _In_opt_ WDFDEVICE Device,
    _In_z_ PCWSTR Stage,
    _In_ NTSTATUS Status)
{
    WDFKEY key;
    UNICODE_STRING valueName;
    UNICODE_STRING value;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "diag %ws status=0x%08x\n",
               Stage,
               Status);
    ViosndWriteServiceDiag(Stage, Status);

    if (Device == NULL) {
        return;
    }

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device,
                                             PLUGPLAY_REGKEY_DEVICE,
                                             KEY_SET_VALUE,
                                             WDF_NO_OBJECT_ATTRIBUTES,
                                             &key))) {
        return;
    }

    RtlInitUnicodeString(&valueName, L"LastInitStage");
    RtlInitUnicodeString(&value, Stage);
    (VOID)WdfRegistryAssignUnicodeString(key, &valueName, &value);

    RtlInitUnicodeString(&valueName, L"LastInitStatus");
    (VOID)WdfRegistryAssignULong(key, &valueName, (ULONG)Status);

    WdfRegistryClose(key);
}

NTSTATUS
ViosndEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PVIOSND_ACX_DEVICE context;
    ACX_DEVICEINIT_CONFIG acxInitConfig;
    ACX_DEVICE_CONFIG acxDeviceConfig;
    WDF_INTERRUPT_CONFIG interruptConfig;

    UNREFERENCED_PARAMETER(Driver);
    ViosndWriteServiceDiag(L"DeviceAdd.Start", STATUS_SUCCESS);

    ACX_DEVICEINIT_CONFIG_INIT(&acxInitConfig);

    ViosndWriteServiceDiag(L"DeviceAdd.BeforeAcxDeviceInitInitialize", STATUS_SUCCESS);
    status = AcxDeviceInitInitialize(DeviceInit, &acxInitConfig);
    if (!NT_SUCCESS(status)) {
        ViosndWriteServiceDiag(L"DeviceAdd.AcxDeviceInitInitialize", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: AcxDeviceInitInitialize failed 0x%08x\n",
                   status);
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = ViosndEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = ViosndEvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = ViosndEvtDeviceD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = ViosndEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VIOSND_ACX_DEVICE);

    ViosndWriteServiceDiag(L"DeviceAdd.BeforeWdfDeviceCreate", STATUS_SUCCESS);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        ViosndWriteServiceDiag(L"DeviceAdd.WdfDeviceCreate", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: WdfDeviceCreate failed 0x%08x\n",
                   status);
        return status;
    }

    context = ViosndGetDeviceContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->WdfDevice = device;
    ViosndAcxLoadLogSwitch(device);
    ViosndAcxWriteDiag(device, L"DeviceAdd.WdfDeviceCreate", STATUS_SUCCESS);

    ACX_DEVICE_CONFIG_INIT(&acxDeviceConfig);
    status = AcxDeviceInitialize(device, &acxDeviceConfig);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.AcxDeviceInitialize", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: AcxDeviceInitialize failed 0x%08x\n",
                   status);
        return status;
    }

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
                              ViosndEvtInterruptIsr,
                              ViosndEvtInterruptDpc);
    status = WdfInterruptCreate(device,
                                &interruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &context->QueueInterrupt);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.WdfInterruptCreate", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: WdfInterruptCreate failed 0x%08x\n",
                   status);
        return status;
    }

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->ControlLock);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.ControlLock", status);
        return status;
    }
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->EventLock);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.EventLock", status);
        return status;
    }
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->TxLock);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.TxLock", status);
        return status;
    }
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->RxLock);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.RxLock", status);
        return status;
    }

    status = ViosndAcxCreateCircuit(device,
                                    FALSE,
                                    0,
                                    &context->RenderCircuit);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.CreateRenderCircuit", status);
        return status;
    }

    status = ViosndAcxCreateCircuit(device,
                                    TRUE,
                                    1,
                                    &context->CaptureCircuit);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(device, L"DeviceAdd.CreateCaptureCircuit", status);
        return status;
    }

    ViosndAcxWriteDiag(device, L"DeviceAdd.Done", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesRaw);
    ViosndAcxWriteDiag(Device, L"PrepareHardware.Start", STATUS_SUCCESS);

    status = VirtIOWdfInitialize(&context->Vio,
                                 Device,
                                 ResourcesTranslated,
                                 context->QueueInterrupt,
                                 VIOSND_POOL_TAG);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"PrepareHardware.VirtIOWdfInitialize", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: VirtIOWdfInitialize failed 0x%08x\n",
                   status);
        return status;
    }

    status = ViosndAcxInitializeVirtio(context);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"PrepareHardware.InitializeVirtio", status);
        VirtIOWdfSetDriverFailed(&context->Vio);
        ViosndAcxDestroyDevice(context);
        return status;
    }

    status = ViosndAcxQueryPcmStreams(context);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"PrepareHardware.QueryPcmStreams", status);
        VirtIOWdfSetDriverFailed(&context->Vio);
        ViosndAcxDestroyDevice(context);
        return status;
    }

    status = ViosndAcxAddStaticCircuits(Device, context);

    if (NT_SUCCESS(status)) {
        VirtIOWdfSetDriverOK(&context->Vio);
        context->VirtioReady = TRUE;
        ViosndAcxWriteDiag(Device, L"PrepareHardware.Done", STATUS_SUCCESS);
    } else {
        ViosndAcxRemoveStaticCircuits(Device, context);
        VirtIOWdfSetDriverFailed(&context->Vio);
        ViosndAcxDestroyDevice(context);
    }

    return status;
}

NTSTATUS
ViosndEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    ViosndAcxWriteDiag(Device, L"ReleaseHardware.Start", STATUS_SUCCESS);
    ViosndAcxRemoveStaticCircuits(Device, context);
    ViosndAcxDestroyDevice(context);
    ViosndAcxWriteDiag(Device, L"ReleaseHardware.Done", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);

    if (context->QueuesReady) {
        return STATUS_SUCCESS;
    }

    status = ViosndAcxInitializeVirtio(context);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"D0Entry.InitializeVirtio", status);
        VirtIOWdfSetDriverFailed(&context->Vio);
        ViosndAcxDestroyDevice(context);
        return status;
    }

    status = ViosndAcxQueryPcmStreams(context);
    if (!NT_SUCCESS(status)) {
        ViosndAcxWriteDiag(Device, L"D0Entry.QueryPcmStreams", status);
        VirtIOWdfSetDriverFailed(&context->Vio);
        ViosndAcxDestroyDevice(context);
        return status;
    }

    VirtIOWdfSetDriverOK(&context->Vio);
    context->VirtioReady = TRUE;
    ViosndAcxWriteDiag(Device, L"D0Entry.Done", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);
    if (context->VirtioReady) {
        ViosndAcxStopStreams(context, TRUE);
        context->VirtioReady = FALSE;
    }

    if (context->QueuesReady) {
        VirtIOWdfDestroyQueues(&context->Vio);
        RtlZeroMemory(context->Queues, sizeof(context->Queues));
        context->QueuesReady = FALSE;
        ViosndAcxFreeEventBuffers(context);
    }
    return STATUS_SUCCESS;
}

BOOLEAN
ViosndEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(device);
    UCHAR isr;

    UNREFERENCED_PARAMETER(MessageID);

    if (!context->VirtioReady) {
        return FALSE;
    }

    isr = VirtIOWdfGetISRStatus(&context->Vio);
    if (isr == 0) {
        return FALSE;
    }

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
ViosndEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

    WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
    PVIOSND_ACX_DEVICE context = ViosndGetDeviceContext(device);

    if (context->VirtioReady) {
        ViosndAcxDrainQueues(context);
    }
}
