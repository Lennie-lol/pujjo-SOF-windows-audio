/* SPDX-License-Identifier: BSD-3-Clause */
/* Pujjo RT5682S / RTL1019P KMDF resource-discovery component.
 * Default: resource discovery only, no SPB/GPIO operations.
 * Optional PJ5682S_ENABLE_READONLY_PROBE: RT5682S device-ID read ONLY,
 * explicitly disabled in all shipped project build configurations.
 * No INF is shipped: do not bind to production PnP devices. */
#include <ntddk.h>
#include <wdf.h>
#include "PujjoBoardGuard.h"
#include "PujjoResourcePolicy.h"

#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
#include "PujjoRt5682sSpbWindows.h"
#endif

#ifndef PJ_TARGET_CODEC
#ifndef PJ_TARGET_AMP
#error Build must explicitly select exactly one target kind
#endif
#endif
#if defined(PJ_TARGET_CODEC) && defined(PJ_TARGET_AMP)
#error Cannot mix codec and amplifier targets
#endif

/* Immutable diagnostic context. Connections are deliberately NOT opened. */
typedef struct _PJ_DEV_CONTEXT {
    pj_summary Evidence;
    BOOLEAN ResourceEvidenceValid;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
    PJ5682S_SPB_WINDOWS Spb;
    pj5682s_codec Codec;
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
    while (pos < total && ids[pos]) {
        ULONG end = pos;
        while (end < total && ids[end]) ++end;
        if (end >= total || end - pos > 0x7FFFu) return FALSE;
        found.Buffer = &ids[pos];
        found.Length = (USHORT)((end - pos) * sizeof(WCHAR));
        found.MaximumLength = found.Length;
        if (RtlEqualUnicodeString(&found, &expected1, TRUE) ||
            RtlEqualUnicodeString(&found, &expected2, TRUE))
            return TRUE;
        pos = end + 1;
    }
    return FALSE;
}

EVT_WDF_DRIVER_DEVICE_ADD PjEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PjEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE PjEvtReleaseHardware;
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
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
#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
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
    if (!PujjoBoardIdentityMatches() || !PjHardwareIdMatches(Device))
        return STATUS_NOT_SUPPORTED;
    n = WdfCmResourceListGetCount(Translated);
    if (n == 0 || n > PJ_MAX_RESOURCES)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(items, sizeof(items));
    for (i = 0; i < n; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d =
            WdfCmResourceListGetDescriptor(Translated, i);
        if (!d) return STATUS_DEVICE_CONFIGURATION_ERROR;
        items[i].kind = PJ_RES_OTHER;
        if (d->Type == CmResourceTypeInterrupt) {
            items[i].kind = PJ_RES_INTERRUPT;
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
        }
    }
#ifdef PJ_TARGET_CODEC
    result = pj_validate_resources(PJ_PROBE_CODEC, items, n, &summary);
#else
    result = pj_validate_resources(PJ_PROBE_AMP, items, n, &summary);
#endif
    if (result != PJ_POLICY_OK) return STATUS_DEVICE_CONFIGURATION_ERROR;
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
        if (!NT_SUCCESS(status)) { ctx->ResourceEvidenceValid=FALSE; return status; }
        Pj5682sSpbBindCallbacks(&ctx->Spb, &bus);
        if (pj5682s_bind(&ctx->Codec, &bus)!=PJ5682S_OK) {
            pj5682s_unbind(&ctx->Codec);
            Pj5682sSpbClose(&ctx->Spb);
            ctx->ResourceEvidenceValid=FALSE;
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
#endif
    /* Having resources is NOT permission to drive an amplifier or load audio
     * firmware. No driver install INF or audio routing exists. */
    return STATUS_SUCCESS;
}

#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)
/* Called after the framework has brought device power to D0. A failed read
 * refuses device startup rather than guessing codec presence. */
NTSTATUS PjEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PJ_DEV_CONTEXT *ctx=PjGetDeviceContext(Device);
    pj5682s_status result;
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
    result=pj5682s_identify(&ctx->Codec);
    if (result!=PJ5682S_OK) (void)Pj5682sSpbSetPower(&ctx->Spb, FALSE);
    return result==PJ5682S_OK ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS PjEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PJ_DEV_CONTEXT *ctx=PjGetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();
    (void)Pj5682sSpbSetPower(&ctx->Spb, FALSE);
    if (ctx->Codec.state==PJ5682S_IDENTIFIED ||
        ctx->Codec.state==PJ5682S_BOUND)
        (void)pj5682s_suspend(&ctx->Codec);
    return STATUS_SUCCESS;
}
#endif

NTSTATUS PjEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
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
}
