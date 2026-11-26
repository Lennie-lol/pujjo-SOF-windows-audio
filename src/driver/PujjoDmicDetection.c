/* SPDX-License-Identifier: BSD-3-Clause */
/* Detection-only PDO. No firmware, DMA, register writes, IRQ, stream or
 * analog dependency. The distinct interface cannot bind a capture driver. */
#include <initguid.h>
#include "Driver.h"
#include "PujjoAudioBus.h"
#include "Nhlt.h"

#ifdef PUJJO_DMIC_DETECTION_TEST
#if !defined(PUJJO_DSP_BOOT_DIAGNOSTIC) || defined(PUJJO_ENABLE_EXPERIMENTAL_AUDIO) || \
    defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG) || defined(PUJJO_DMIC_ENUMERATION_CANDIDATE)
#error Detection test must be built on the isolated resource-only diagnostic
#endif

#define DETECTION_INTERFACE_TAG 'tDmP'
typedef struct _PUJJO_DETECTION_CONTEXT {
    volatile LONG Online;
    PUJJO_AUDIO_FORMAT Format;
} PUJJO_DETECTION_CONTEXT, *PPUJJO_DETECTION_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PUJJO_DETECTION_CONTEXT, PujjoDetectionContext)

static VOID DetectionReference(PVOID Context)
{
    WdfObjectReferenceWithTag((WDFDEVICE)Context, (PVOID)(ULONG_PTR)DETECTION_INTERFACE_TAG);
}

static VOID DetectionDereference(PVOID Context)
{
    WdfObjectDereferenceWithTag((WDFDEVICE)Context, (PVOID)(ULONG_PTR)DETECTION_INTERFACE_TAG);
}

static NTSTATUS DetectionFormat(PVOID Context, PUJJO_AUDIO_FORMAT *Format)
{
    if (!Context || !Format) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Format, sizeof(*Format));
    PPUJJO_DETECTION_CONTEXT state = PujjoDetectionContext((WDFDEVICE)Context);
    if (InterlockedCompareExchange(&state->Online, 0, 0) != 1)
        return STATUS_DEVICE_NOT_READY;
    *Format = state->Format; /* Immutable metadata, not capture availability. */
    return STATUS_SUCCESS;
}

static NTSTATUS DetectionAllocate(PVOID Context, ULONG RequestedBytes, PUJJO_AUDIO_BUFFER *Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(RequestedBytes);
    if (Buffer) RtlZeroMemory(Buffer, sizeof(*Buffer));
    return STATUS_DEVICE_NOT_READY;
}
static VOID DetectionFree(PVOID Context, PVOID Stream)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Stream);
}
static NTSTATUS DetectionSetState(PVOID Context, PVOID Stream, ULONG State)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Stream);
    UNREFERENCED_PARAMETER(State);
    return STATUS_DEVICE_NOT_READY;
}
static NTSTATUS DetectionPosition(PVOID Context, PVOID Stream, PULONG Position)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Stream);
    if (Position) *Position = 0;
    return STATUS_DEVICE_NOT_READY;
}
static NTSTATUS DetectionNotification(PVOID Stream, PKEVENT Event, BOOLEAN Add)
{
    UNREFERENCED_PARAMETER(Stream);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Add);
    return STATUS_DEVICE_NOT_READY;
}

/* A successful native query must pass the strict profile validator.
 * Fallback is valid only for a genuinely absent native table. */
static NTSTATUS DetectionSelectProfile(pj_dmic_profile *Profile)
{
    PVOID table = NULL;
    ULONG bytes = 0;
    NTSTATUS status = SofNhltQueryAcpiTable(&table, &bytes);
    if (NT_SUCCESS(status))
        status = SofNhltInspectPujjoDmic(table, bytes, Profile);
    else if (status == STATUS_NOT_FOUND || status == STATUS_OBJECT_NAME_NOT_FOUND)
        status = SofNhltInspectEmbeddedDmic(Profile);
    SofNhltFreeAcpiTable(table);
    return status;
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP DetectionCleanup;
static VOID DetectionCleanup(WDFOBJECT Object)
{
    InterlockedExchange(&PujjoDetectionContext((WDFDEVICE)Object)->Online, 0);
}

NTSTATUS SofPublishDmicDetectionChild(PSOF_DEVICE_CONTEXT Ctx)
{
    DECLARE_CONST_UNICODE_STRING(hardwareId, PUJJO_DMIC_DETECTION_ID);
    DECLARE_CONST_UNICODE_STRING(instanceId, L"DMIC-DETECTION-TEST");
    DECLARE_CONST_UNICODE_STRING(description, L"Pujjo Internal Microphone (Detection test - recording disabled)");
    DECLARE_CONST_UNICODE_STRING(location, L"Pujjo internal digital microphone");
    pj_dmic_profile profile;
    PWDFDEVICE_INIT init;
    WDFDEVICE child = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_QUERY_INTERFACE_CONFIG config;
    PUJJO_AUDIO_BUS bus = {0};
    NTSTATUS status;

    /* This is reached only after the diagnostic's one-shot, SMBIOS, PCI
     * identity/resource checks and complete BAR unmap. */
    if (!Ctx || Ctx->VendorId != 0x8086 || Ctx->DeviceId != 0x54c8 ||
        !Ctx->NumCaptureStreams || Ctx->AudioChild ||
        Ctx->Bar0VirtualAddress || Ctx->Bar4VirtualAddress ||
        Ctx->DmaEnabler || Ctx->WdfInterrupt)
        return STATUS_DEVICE_NOT_READY;
    status = DetectionSelectProfile(&profile);
    if (!NT_SUCCESS(status)) return status;

    init = WdfPdoInitAllocate(Ctx->WdfDevice);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    status = WdfPdoInitAssignDeviceID(init, &hardwareId);
    if (NT_SUCCESS(status)) status = WdfPdoInitAddHardwareID(init, &hardwareId);
    if (NT_SUCCESS(status)) status = WdfPdoInitAssignInstanceID(init, &instanceId);
    if (NT_SUCCESS(status)) status = WdfPdoInitAddDeviceText(init, &description, &location, 0x409);
    if (NT_SUCCESS(status)) {
        WdfPdoInitSetDefaultLocale(init, 0x409);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PUJJO_DETECTION_CONTEXT);
        attributes.EvtCleanupCallback = DetectionCleanup;
        status = WdfDeviceCreate(&init, &attributes, &child);
    }
    if (!NT_SUCCESS(status)) {
        if (init) WdfDeviceInitFree(init);
        return status;
    }

    /* Interface refs hold the PDO itself, never a parent DMA owner.
     * No parent address is retained by this immutable metadata bus. */
    PPUJJO_DETECTION_CONTEXT state = PujjoDetectionContext(child);
    state->Format.Rate = PJ_DMIC_RATE;
    state->Format.Channels = PJ_DMIC_CHANNELS;
    state->Format.ContainerBits = PJ_DMIC_BITS;
    state->Format.ValidBits = PJ_DMIC_BITS;
    state->Format.Role = PujjoAudioDmic;
    bus.Header.Size = (USHORT)sizeof(bus);
    bus.Header.Version = PUJJO_AUDIO_INTERFACE_VERSION;
    bus.Header.Context = child;
    bus.Header.InterfaceReference = DetectionReference;
    bus.Header.InterfaceDereference = DetectionDereference;
    bus.GetFormat = DetectionFormat;
    bus.Allocate = DetectionAllocate;
    bus.Free = DetectionFree;
    bus.SetState = DetectionSetState;
    bus.Position = DetectionPosition;
    bus.Notification = DetectionNotification;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&config, &bus.Header, &GUID_PUJJO_DMIC_DETECTION_BUS, NULL);
    status = WdfDeviceAddQueryInterface(child, &config);
    if (NT_SUCCESS(status)) {
        InterlockedExchange(&state->Online, 1);
        status = WdfFdoAddStaticChild(Ctx->WdfDevice, child);
    }
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&state->Online, 0);
        WdfObjectDelete(child);
        return status;
    }
    /* Sentinel only; removal never dereferences this cached handle. */
    Ctx->AudioChild = child;
    return STATUS_SUCCESS;
}

/* Use framework-locked iteration instead of dereferencing an unpinned
 * cached PDO handle, including a child independently removed by PnP. */
VOID SofWithdrawDmicDetectionChild(WDFDEVICE Device)
{
    WDFDEVICE child = NULL;
    WdfFdoLockStaticChildListForIteration(Device);
    while ((child = WdfFdoRetrieveNextStaticChild(Device, child, WdfRetrieveAddedChildren)) != NULL) {
        InterlockedExchange(&PujjoDetectionContext(child)->Online, 0);
        NTSTATUS status = WdfPdoMarkMissing(child);
        if (!NT_SUCCESS(status))
            SofLogBreadcrumb("[Detection] Child withdrawal failed; capture remains blocked.\n");
        break; /* This variant can publish just one child. */
    }
    WdfFdoUnlockStaticChildListFromIteration(Device);
    GetDeviceContext(Device)->AudioChild = NULL;
}
VOID SofEvtDetectionSurpriseRemoval(WDFDEVICE Device)
{
    SofWithdrawDmicDetectionChild(Device);
}
#endif
