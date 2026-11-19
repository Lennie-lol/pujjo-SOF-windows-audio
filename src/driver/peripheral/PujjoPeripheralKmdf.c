/* SPDX-License-Identifier: BSD-3-Clause */
/* Pujjo RT5682S / RTL1019P KMDF resource-discovery component.
 * Default: resource discovery only, no SPB/GPIO operations.
 * Optional PJ5682S_ENABLE_READONLY_PROBE: RT5682S device-ID read ONLY,
 * explicitly disabled in all shipped project build configurations.
 * Runtime interfaces are built separately from the diagnostics. */
#include <ntddk.h>
#include <wdf.h>
#include "PujjoBoardGuard.h"
#include "PujjoResourcePolicy.h"

#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
#include "PujjoRt5682sSpbWindows.h"
#endif

#if defined(PJ5682S_ENABLE_CONTROL) && defined(PJ5682S_ENABLE_READONLY_PROBE)
#error Resource/identity diagnostics and runtime control must use distinct builds
#endif
#if (defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_CONTROL)) || \
    (defined(PJ_TARGET_AMP) && defined(PJ1019_ENABLE_CONTROL))
#define PJ_PERIPHERAL_RUNTIME
#include <initguid.h>
#include "PujjoPeripheralInterface.h"
#ifdef PJ_TARGET_CODEC
#include "PujjoRt5682sSpbWindows.h"
#else
#include "PujjoRt1019pGpioWindows.h"
#endif
#endif

#ifndef PJ_TARGET_CODEC
#ifndef PJ_TARGET_AMP
#error Build must explicitly select exactly one target kind
#endif
#endif
#if defined(PJ_TARGET_CODEC) && defined(PJ_TARGET_AMP)
#error Cannot mix codec and amplifier targets
#endif

/* Resource-discovery and optional runtime state. */
typedef struct _PJ_DEV_CONTEXT {
    pj_summary Evidence;
    BOOLEAN ResourceEvidenceValid;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
    PJ5682S_SPB_WINDOWS Spb;
    pj5682s_codec Codec;
#endif
#ifdef PJ_PERIPHERAL_RUNTIME
    WDFDEVICE Device;
    KMUTEX RuntimeLock;
    BOOLEAN Online;
#ifdef PJ_TARGET_CODEC
    PJ5682S_SPB_WINDOWS RuntimeSpb;
    BOOLEAN RuntimeConnectionKnown;
    ULONG RuntimeConnectionLow;
    ULONG RuntimeConnectionHigh;
#else
    PJ1019_GPIO_WINDOWS RuntimeGpio;
#endif
#endif
} PJ_DEV_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PJ_DEV_CONTEXT, PjGetDeviceContext)

/* PnP hardware ID is a bounded MULTI_SZ string. Match the specific Realtek
 * node as well as the Pujjo SMBIOS board; a matching resource type alone is
 * insufficient to authorize a driver to claim the device. */
static BOOLEAN PjHardwareIdMatches(WDFDEVICE Device)
{
    WCHAR ids[256] = {0};
    ULONG bytes = 0, total, pos = 0;
    NTSTATUS status;
    BOOLEAN matched = FALSE;
    UNICODE_STRING expected1, expected2, found;
#ifdef PJ_TARGET_CODEC
    RtlInitUnicodeString(&expected1, L"ACPI\\RTL5682");
    RtlInitUnicodeString(&expected2, L"ACPI\\VEN_RTL&DEV_5682");
#else
    RtlInitUnicodeString(&expected1, L"ACPI\\RTL1019");
    RtlInitUnicodeString(&expected2, L"ACPI\\VEN_RTL&DEV_1019");
#endif
    status = WdfDeviceQueryProperty(Device, DevicePropertyHardwareID,
                                    sizeof(ids), ids, &bytes);
    if (!NT_SUCCESS(status) || bytes < 2 * sizeof(WCHAR) ||
        bytes > sizeof(ids) || (bytes % sizeof(WCHAR)) != 0)
        return FALSE;
    total = bytes / sizeof(WCHAR);
    /* Validate the complete MULTI_SZ, not merely its first matching entry. */
    if (ids[total - 1] != 0 || ids[total - 2] != 0) return FALSE;
    while (pos < total && ids[pos]) {
        ULONG end = pos;
        while (end < total && ids[end]) ++end;
        if (end >= total || end - pos > 0x7FFFu) return FALSE;
        found.Buffer = &ids[pos];
        found.Length = (USHORT)((end - pos) * sizeof(WCHAR));
        found.MaximumLength = found.Length;
        if (RtlEqualUnicodeString(&found, &expected1, TRUE) ||
            RtlEqualUnicodeString(&found, &expected2, TRUE))
            matched = TRUE;
        pos = end + 1;
    }
    /* Allow trailing NUL padding, but no hidden strings after termination. */
    while (pos < total) {
        if (ids[pos++] != 0) return FALSE;
    }
    return matched;
}

#ifdef PJ_PERIPHERAL_RUNTIME
static VOID RuntimeLock(PJ_DEV_CONTEXT *ctx)
{
    KeWaitForSingleObject(&ctx->RuntimeLock, Executive, KernelMode, FALSE, NULL);
}

static VOID RuntimeUnlock(PJ_DEV_CONTEXT *ctx)
{
    KeReleaseMutex(&ctx->RuntimeLock, FALSE);
}

static VOID RuntimeReference(PVOID context)
{
    PJ_DEV_CONTEXT *ctx = context;
    WdfObjectReference(ctx->Device);
}

static VOID RuntimeDereference(PVOID context)
{
    PJ_DEV_CONTEXT *ctx = context;
    WdfObjectDereference(ctx->Device);
}

static NTSTATUS RuntimePrepare(PVOID context, const PUJJO_PERIPHERAL_PROFILE *profile)
{
    PJ_DEV_CONTEXT *ctx = context;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;

    if (!profile || profile->BoardQualified != 1u || profile->DspMuted != 1u)
        return STATUS_ACCESS_DENIED;
    RuntimeLock(ctx);
    if (!ctx->Online || !ctx->ResourceEvidenceValid) goto Done;
#ifdef PJ_TARGET_CODEC
    if (ctx->RuntimeConnectionKnown &&
        (ctx->Evidence.connection_low != ctx->RuntimeConnectionLow ||
         ctx->Evidence.connection_high != ctx->RuntimeConnectionHigh)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Done;
    }
    if (!ctx->RuntimeSpb.Opened) {
        status = Pj5682sSpbOpen(ctx->Device, ctx->Evidence.connection_low,
                              ctx->Evidence.connection_high, &ctx->RuntimeSpb);
        if (!NT_SUCCESS(status)) goto Done;
        ctx->RuntimeConnectionKnown = TRUE;
        ctx->RuntimeConnectionLow = ctx->Evidence.connection_low;
        ctx->RuntimeConnectionHigh = ctx->Evidence.connection_high;
    }
    status = Pj5682sSpbSetPower(&ctx->RuntimeSpb, TRUE);
    if (NT_SUCCESS(status))
        status = Pj5682sSpbStartControl(&ctx->RuntimeSpb, &profile->Codec, &profile->CodecAnalog);
#else
    if (profile->GainLimitApplied != 1u || (profile->SdbPolarity != Pj1019SdbActiveLow &&
        profile->SdbPolarity != Pj1019SdbActiveHigh) || profile->CalibrationMs < 300u ||
        profile->CalibrationMs > 2000u) {
        status = STATUS_ACCESS_DENIED;
        goto Done;
    }
    if (!ctx->RuntimeGpio.Opened) {
        status = Pj1019GpioOpen(ctx->Device, ctx->Evidence.connection_low,
            ctx->Evidence.connection_high, (PJ1019_SDB_POLARITY)profile->SdbPolarity,
            &ctx->RuntimeGpio);
        if (!NT_SUCCESS(status)) goto Done;
    } else if ((unsigned)ctx->RuntimeGpio.Polarity != profile->SdbPolarity) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Done;
    }
    status = Pj1019GpioSetPower(&ctx->RuntimeGpio, TRUE);
    if (NT_SUCCESS(status)) {
        const PJ1019_RUNTIME_PROFILE amp = {TRUE, TRUE, TRUE, profile->CalibrationMs};
        status = Pj1019GpioStartMuted(&ctx->RuntimeGpio, &amp);
    }
#endif
Done:
    RuntimeUnlock(ctx);
    return status;
}

static NTSTATUS RuntimeShutdown(PVOID context)
{
    PJ_DEV_CONTEXT *ctx = context;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    RuntimeLock(ctx);
    if (ctx->Online && ctx->ResourceEvidenceValid) {
#ifdef PJ_TARGET_CODEC
        status = Pj5682sSpbStopControl(&ctx->RuntimeSpb);
#else
        status = ctx->RuntimeGpio.Powered ? Pj1019GpioShutdown(&ctx->RuntimeGpio) :
                 ctx->RuntimeGpio.Faulted ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
#endif
    }
    RuntimeUnlock(ctx);
    return status;
}

#ifdef PJ_TARGET_CODEC
static NTSTATUS RuntimeReadJack(PVOID context, BOOLEAN ready, pj5682s_jack *jack)
{
    PJ_DEV_CONTEXT *ctx = context;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    RuntimeLock(ctx);
    if (ctx->Online && ctx->ResourceEvidenceValid)
        status = Pj5682sSpbGetJack(&ctx->RuntimeSpb, ready, jack);
    RuntimeUnlock(ctx);
    return status;
}
#endif

static NTSTATUS RuntimeInitialize(WDFDEVICE device)
{
    PJ_DEV_CONTEXT *ctx = PjGetDeviceContext(device);
    WDF_QUERY_INTERFACE_CONFIG config;
    PUJJO_PERIPHERAL_INTERFACE bus = {0};
    const GUID *deviceInterface;
    NTSTATUS status;
    ctx->Device = device;
    KeInitializeMutex(&ctx->RuntimeLock, 0);
#ifdef PJ_TARGET_CODEC
    status = Pj5682sSpbInitialize(device, &ctx->RuntimeSpb);
    deviceInterface = &GUID_DEVINTERFACE_PUJJO_CODEC_CONTROL;
    bus.Kind = PUJJO_CONTROL_CODEC;
    bus.ReadJack = RuntimeReadJack;
#else
    status = Pj1019GpioInitialize(device, &ctx->RuntimeGpio);
    deviceInterface = &GUID_DEVINTERFACE_PUJJO_AMP_CONTROL;
    bus.Kind = PUJJO_CONTROL_AMP;
#endif
    if (!NT_SUCCESS(status)) return status;
    bus.Header.Size = (USHORT)sizeof(bus);
    bus.Header.Version = PUJJO_PERIPHERAL_CONTROL_VERSION;
    bus.Header.Context = ctx;
    bus.Header.InterfaceReference = RuntimeReference;
    bus.Header.InterfaceDereference = RuntimeDereference;
    bus.Prepare = RuntimePrepare;
    bus.Shutdown = RuntimeShutdown;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&config, &bus.Header, &GUID_PUJJO_PERIPHERAL_CONTROL, NULL);
    status = WdfDeviceAddQueryInterface(device, &config);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateDeviceInterface(device, deviceInterface, NULL);
}

static NTSTATUS RuntimeDisconnect(PJ_DEV_CONTEXT *ctx, BOOLEAN release)
{
    NTSTATUS status, closeStatus = STATUS_SUCCESS;
    RuntimeLock(ctx);
    ctx->Online = FALSE;
#ifdef PJ_TARGET_CODEC
    status = Pj5682sSpbSetPower(&ctx->RuntimeSpb, FALSE);
    if (release) closeStatus = Pj5682sSpbClose(&ctx->RuntimeSpb);
#else
    status = Pj1019GpioSetPower(&ctx->RuntimeGpio, FALSE);
    if (release) closeStatus = Pj1019GpioClose(&ctx->RuntimeGpio);
#endif
    if (release) ctx->ResourceEvidenceValid = FALSE;
    RuntimeUnlock(ctx);
    return NT_SUCCESS(status) ? closeStatus : status;
}
#endif

EVT_WDF_DRIVER_DEVICE_ADD PjEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PjEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE PjEvtReleaseHardware;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE) || defined(PJ_PERIPHERAL_RUNTIME)
EVT_WDF_DEVICE_D0_ENTRY PjEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT PjEvtD0Exit;
#endif

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PjEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS PjEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PjEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = PjEvtReleaseHardware;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE) || defined(PJ_PERIPHERAL_RUNTIME)
    pnp.EvtDeviceD0Entry = PjEvtD0Entry;
    pnp.EvtDeviceD0Exit = PjEvtD0Exit;
#endif
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PJ_DEV_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (NT_SUCCESS(status))
        RtlZeroMemory(PjGetDeviceContext(device), sizeof(PJ_DEV_CONTEXT));
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
    if (NT_SUCCESS(status))
        status = Pj5682sSpbInitialize(device, &PjGetDeviceContext(device)->Spb);
#endif
#ifdef PJ_PERIPHERAL_RUNTIME
    if (NT_SUCCESS(status)) status = RuntimeInitialize(device);
#endif
    return status;
}

NTSTATUS PjEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                              WDFCMRESLIST Translated)
{
    pj_resource items[PJ_MAX_RESOURCES];
    pj_summary summary;
    ULONG n, i;
    enum pj_policy_result result;
    PJ_DEV_CONTEXT *ctx = PjGetDeviceContext(Device);
    UNREFERENCED_PARAMETER(Raw);
    PAGED_CODE();
    RtlZeroMemory(&ctx->Evidence, sizeof(ctx->Evidence));
    ctx->ResourceEvidenceValid = FALSE;
    /* Board guard uses SMBIOS Google + Pujjo/Pujjo1e, must fail closed. */
    if (!PujjoBoardIdentityMatches() || !PjHardwareIdMatches(Device)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[PujjoProbe] Board or peripheral hardware ID rejected\n");
        return STATUS_NOT_SUPPORTED;
    }
    n = WdfCmResourceListGetCount(Translated);
    if (n == 0 || n > PJ_MAX_RESOURCES) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[PujjoProbe] Invalid translated resource count: %lu\n", n);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(items, sizeof(items));
    for (i = 0; i < n; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d =
            WdfCmResourceListGetDescriptor(Translated, i);
        if (!d) return STATUS_DEVICE_CONFIGURATION_ERROR;
        items[i].kind = PJ_RES_OTHER;
        if (d->Type == CmResourceTypeInterrupt) {
            items[i].kind = PJ_RES_INTERRUPT;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "[PujjoProbe] Resource %lu: interrupt flags=0x%x\n", i, (ULONG)d->Flags);
        } else if (d->Type == CmResourceTypeConnection) {
            items[i].id_low = d->u.Connection.IdLowPart;
            items[i].id_high = d->u.Connection.IdHighPart;
            if (d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
                items[i].kind = PJ_RES_I2C;
            else if (d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
                     d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO)
                items[i].kind = PJ_RES_GPIO_IO;
            else items[i].kind = PJ_RES_UNKNOWN_CONNECTION;
            /* These values are opaque connection IDs, not pin/address data. */
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "[PujjoProbe] Resource %lu: connection class=%u type=%u id=%08lx:%08lx\n",
                i, (ULONG)d->u.Connection.Class, (ULONG)d->u.Connection.Type,
                (ULONG)items[i].id_high, (ULONG)items[i].id_low);
        }
    }
#ifdef PJ_TARGET_CODEC
    result = pj_validate_resources(PJ_PROBE_CODEC, items, n, &summary);
#else
    result = pj_validate_resources(PJ_PROBE_AMP, items, n, &summary);
#endif
    if (result != PJ_POLICY_OK) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[PujjoProbe] Resource policy rejected: %u\n", (ULONG)result);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "[PujjoProbe] Resource policy accepted: I2C=%lu GPIO=%lu IRQ=%lu\n",
        (ULONG)summary.i2c_count, (ULONG)summary.gpio_count, (ULONG)summary.interrupt_count);
    ctx->Evidence = summary;
    ctx->ResourceEvidenceValid = TRUE;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
    {
        pj5682s_bus bus;
        NTSTATUS status;
        /* Second opt-in stage after board, ID and resource validation:
         * opens the Resource Hub target, but delays read until D0Entry
         * where Windows reports the device powered. NEVER writes. */
        status=Pj5682sSpbOpen(Device, summary.connection_low,
                              summary.connection_high, &ctx->Spb);
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[PujjoProbe] SPB open failed: 0x%08lx\n", (ULONG)status);
            ctx->ResourceEvidenceValid=FALSE;
            return status;
        }
        Pj5682sSpbBindCallbacks(&ctx->Spb, &bus);
        if (pj5682s_bind(&ctx->Codec, &bus)!=PJ5682S_OK) {
            pj5682s_unbind(&ctx->Codec);
            Pj5682sSpbClose(&ctx->Spb);
            ctx->ResourceEvidenceValid=FALSE;
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
#endif
    /* Discovery never authorizes route activation. */
    return STATUS_SUCCESS;
}

#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
/* Called after the framework has brought device power to D0. A failed read
 * refuses device startup rather than guessing codec presence. */
NTSTATUS PjEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PJ_DEV_CONTEXT *ctx=PjGetDeviceContext(Device);
    pj5682s_status result;
    pj5682s_identity identity={0};
    UNREFERENCED_PARAMETER(PreviousState);
    PAGED_CODE();
    if (!ctx->ResourceEvidenceValid || !ctx->Spb.Opened)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!NT_SUCCESS(Pj5682sSpbSetPower(&ctx->Spb, TRUE)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (ctx->Codec.state==PJ5682S_ASLEEP &&
        pj5682s_resume(&ctx->Codec)!=PJ5682S_OK) {
        (void)Pj5682sSpbSetPower(&ctx->Spb, FALSE);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    result=pj5682s_collect_identity(&ctx->Codec, &identity);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, result==PJ5682S_OK ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
        "[PujjoProbe] RT5682S identity status=%ld device=0x%04x vendor=0x%04x version=0x%04x\n",
        (LONG)result, (ULONG)identity.device_id, (ULONG)identity.vendor_id, (ULONG)identity.version);
    if (result!=PJ5682S_OK) (void)Pj5682sSpbSetPower(&ctx->Spb, FALSE);
    return result==PJ5682S_OK ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS PjEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PJ_DEV_CONTEXT *ctx=PjGetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();
    NTSTATUS status = Pj5682sSpbSetPower(&ctx->Spb, FALSE);
    if (!NT_SUCCESS(status)) return status;
    if (ctx->Codec.state == PJ5682S_FAILED) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (ctx->Codec.state != PJ5682S_ASLEEP &&
        pj5682s_suspend(&ctx->Codec) != PJ5682S_OK)
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}
#endif

#ifdef PJ_PERIPHERAL_RUNTIME
NTSTATUS PjEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PJ_DEV_CONTEXT *ctx = PjGetDeviceContext(Device);
    NTSTATUS status = STATUS_DEVICE_CONFIGURATION_ERROR;
    UNREFERENCED_PARAMETER(PreviousState);
    RuntimeLock(ctx);
    if (ctx->ResourceEvidenceValid) {
        ctx->Online = TRUE;
        status = STATUS_SUCCESS;
    }
    RuntimeUnlock(ctx);
    return status;
}

NTSTATUS PjEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    return RuntimeDisconnect(PjGetDeviceContext(Device), FALSE);
}
#endif

NTSTATUS PjEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
#ifdef PJ_PERIPHERAL_RUNTIME
    return RuntimeDisconnect(PjGetDeviceContext(Device), TRUE);
#else
    PAGED_CODE();
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
    {
        PJ_DEV_CONTEXT *ctx=PjGetDeviceContext(Device);
        pj5682s_unbind(&ctx->Codec);
        Pj5682sSpbClose(&ctx->Spb);
    }
#endif
    RtlZeroMemory(&PjGetDeviceContext(Device)->Evidence, sizeof(pj_summary));
    PjGetDeviceContext(Device)->ResourceEvidenceValid = FALSE;
    return STATUS_SUCCESS;
#endif
}
