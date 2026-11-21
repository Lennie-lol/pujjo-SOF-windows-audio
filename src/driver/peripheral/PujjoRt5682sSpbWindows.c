/* SPDX-License-Identifier: BSD-3-Clause */
/* Optional KMDF Resource Hub I2C transport. Not hooked into PnP by default.
 * No address, GPIO pin, or bus speed is hard-coded. Caller must validate
 * Pujjo SMBIOS + RTL5682 PnP ID + translated I2C resource BEFORE opening.
 * All entry points require PASSIVE_LEVEL. Windows WDK build NOT verified. */
/* Enable the inline Resource Hub path helpers before any WDK headers. */
#define RESHUB_USE_HELPER_ROUTINES
#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>
#include <spb.h>
#include "PujjoRt5682sSpbWindows.h"
#define PJ5682S_SPB_TIMEOUT_MS 1000

#ifdef PJ5682S_ENABLE_CONTROL
static NTSTATUS StopControlLocked(PJ5682S_SPB_WINDOWS *state);
static int ControlDelay(void *context, unsigned milliseconds);
#endif

NTSTATUS Pj5682sSpbInitialize(WDFDEVICE device, PJ5682S_SPB_WINDOWS *state)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    PAGED_CODE();
    if (!device || !state || state->Lock || state->Target)
        return STATUS_INVALID_PARAMETER;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &state->Lock);
    if (NT_SUCCESS(status)) {
        state->Owner = device;
        state->Opened = FALSE;
        state->Powered = FALSE;
#ifdef PJ5682S_ENABLE_CONTROL
        /* DeviceAdd is the only valid place to initialize this context. */
        RtlZeroMemory(&state->Cold, sizeof(state->Cold));
        pj5682s_cold_invalidate(&state->Cold);
#endif
    }
    return status;
}
NTSTATUS Pj5682sSpbSetPower(PJ5682S_SPB_WINDOWS *state, BOOLEAN powered)
{
    NTSTATUS status = STATUS_SUCCESS;
    PAGED_CODE();
    if (!state || !state->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (powered && !state->Opened) status = STATUS_INVALID_DEVICE_STATE;
    else {
#ifdef PJ5682S_ENABLE_CONTROL
        if (state->Powered && !powered) status = StopControlLocked(state);
        if (!powered) {
            /* D3 is not proof of a codec rail reset: never retain READY. */
            pj5682s_cold_invalidate(&state->Cold);
        }
        if (!state->Powered && powered && state->Control.count) {
            /* Never replay an old epoch's saved registers after power loss.
             * Keep the fault latched until a separately proven reset. */
            state->Control.faulted = 1;
            pj5682s_cold_invalidate(&state->Cold);
            status = STATUS_DEVICE_NOT_READY;
        }
#endif
        state->Powered = powered && NT_SUCCESS(status);
    }
    WdfWaitLockRelease(state->Lock);
    return status;
}

NTSTATUS Pj5682sSpbOpen(WDFDEVICE device, ULONG idLow, ULONG idHigh,
                        PJ5682S_SPB_WINDOWS *state)
{
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_IO_TARGET_OPEN_PARAMS params;
    WDFIOTARGET target = WDF_NO_HANDLE;
    NTSTATUS status;
    PAGED_CODE();
    if (!device || !state || !state->Lock || state->Owner != device)
        return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (state->Opened || state->Target) {
        WdfWaitLockRelease(state->Lock);
        return STATUS_INVALID_DEVICE_STATE;
    }
    /* idLow and idHigh must be the SINGLE validated translated I2C connection
     * ID, not raw ACPI address 0x1a and not a GPIO connection. */
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, idLow, idHigh);
    if (!NT_SUCCESS(status)) { WdfWaitLockRelease(state->Lock); return status; }
    status = WdfIoTargetCreate(device, WDF_NO_OBJECT_ATTRIBUTES, &target);
    if (!NT_SUCCESS(status)) { WdfWaitLockRelease(state->Lock); return status; }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &params, &path, GENERIC_READ | GENERIC_WRITE);
    status = WdfIoTargetOpen(target, &params);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(target);
        WdfWaitLockRelease(state->Lock);
        return status;
    }
    state->Target=target;
    state->Opened=TRUE;
    state->Powered = FALSE;
#ifdef PJ5682S_ENABLE_CONTROL
    pj5682s_cold_invalidate(&state->Cold);
#endif
    WdfWaitLockRelease(state->Lock);
    return STATUS_SUCCESS;
}
NTSTATUS Pj5682sSpbClose(PJ5682S_SPB_WINDOWS *state)
{
    NTSTATUS status = STATUS_SUCCESS;
    PAGED_CODE();
    if (!state || !state->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(state->Lock, NULL);
#ifdef PJ5682S_ENABLE_CONTROL
    if (state->Control.count)
        status = state->Powered ? StopControlLocked(state) : STATUS_DEVICE_NOT_READY;
#endif
    state->Powered = FALSE;
#ifdef PJ5682S_ENABLE_CONTROL
    pj5682s_cold_invalidate(&state->Cold);
#endif
    if (state->Target) {
        WdfIoTargetClose(state->Target);
        WdfObjectDelete(state->Target);
    }
    state->Target=WDF_NO_HANDLE;
    state->Opened=FALSE;
    WdfWaitLockRelease(state->Lock);
    return status;
}

/* reg16 then 2-byte repeated-start read (single atomic SPB sequence). */
static int Pj5682sSpbRead(void *user, const PJ_BYTE *tx, PJ_SIZE txLen,
                         PJ_BYTE *rx, PJ_SIZE rxLen)
{
    PJ5682S_SPB_WINDOWS *state=(PJ5682S_SPB_WINDOWS *)user;
    ULONG_PTR transferred = 0;
    PJ_BYTE observed[2] = {0};
    SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    PAGED_CODE();
    if (!state || !state->Lock || !tx || txLen!=2 || !rx || rxLen!=2) return -1;
    /* Independently enforce the identity-register whitelist at the actual
     * transport boundary, even if a future caller bypasses the codec core. */
    if (tx[0] != 0 || (tx[1] != PJ5682S_DEVICE_ID_REGISTER &&
        tx[1] != PJ5682S_VENDOR_ID_REGISTER && tx[1] != PJ5682S_VERSION_REGISTER))
        return -1;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (!state->Opened || !state->Target || !state->Powered) {
        WdfWaitLockRelease(state->Lock); return -1;
    }
    SPB_TRANSFER_LIST_INIT(&sequence.List, 2);
    sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, (PVOID)tx, (ULONG)txLen);
    sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, observed, (ULONG)sizeof(observed));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &descriptor, &sequence, sizeof(sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ5682S_SPB_TIMEOUT_MS);
    status = WdfIoTargetSendIoctlSynchronously(
        state->Target, WDF_NO_HANDLE, IOCTL_SPB_EXECUTE_SEQUENCE,
        &descriptor, NULL, &options, &transferred);
    /* Never expose partial or failed identity reads to the caller. */
    if (!NT_SUCCESS(status) || transferred != txLen + rxLen) {
        WdfWaitLockRelease(state->Lock);
        return -1;
    }
    rx[0] = observed[0];
    rx[1] = observed[1];
    WdfWaitLockRelease(state->Lock);
    return 0;
}

void Pj5682sSpbBindCallbacks(PJ5682S_SPB_WINDOWS *state, pj5682s_bus *bus)
{
    if (!bus) return;
    RtlZeroMemory(bus, sizeof(*bus));
    PAGED_CODE();
    if (!state || !state->Lock) return;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (!state->Opened || !state->Target) { WdfWaitLockRelease(state->Lock); return; }
    bus->context=state;
    bus->write_then_read=Pj5682sSpbRead;
    /* No physical register-write callback in the Windows transport. */
    bus->write_only=NULL;
    WdfWaitLockRelease(state->Lock);
}

#ifdef PJ5682S_ENABLE_CONTROL
/* Private transport for typed operations. Diagnostic callbacks retain the
 * original identity-only whitelist and cannot reach these writes. */
static int ControlRegister(PJ_WORD reg)
{
    switch (reg) {
    case 0x0010: case 0x0026: case 0x0029: case 0x002b: case 0x0061: case 0x0070:
    case 0x0073: case 0x007b: case 0x007e: case 0x0080:
    case 0x009a: case 0x009b: case 0x009d: case 0x009e:
        return 1;
    default:
        return pj5682s_analog_mask(reg) != 0;
    }
}

static int ControlRead(void *context, const PJ_BYTE *address, PJ_SIZE addressBytes,
                        PJ_BYTE *data, PJ_SIZE dataBytes)
{
    PJ5682S_SPB_WINDOWS *state = context;
    SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR transferred = 0;
    PJ_BYTE result[2] = {0};
    NTSTATUS status;
    PJ_WORD reg;

    if (!state->Opened || !state->Powered || !state->Target ||
        !address || !data || addressBytes != 2 || dataBytes != 2)
        return -1;
    reg = pj5682s_decode_value(address);
    if (!ControlRegister(reg) && reg != PJ5682S_DEVICE_ID_REGISTER &&
        reg != 0x00f0u && reg != 0x0011u && reg != 0x00a0u)
        return -1;

    SPB_TRANSFER_LIST_INIT(&sequence.List, 2);
    sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, (PVOID)address, 2);
    sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, result, sizeof(result));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, &sequence, sizeof(sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ5682S_SPB_TIMEOUT_MS);
    status = WdfIoTargetSendIoctlSynchronously(state->Target, WDF_NO_HANDLE,
        IOCTL_SPB_EXECUTE_SEQUENCE, &descriptor, NULL, &options, &transferred);
    if (!NT_SUCCESS(status) || transferred != sizeof(result) + addressBytes)
        return -1;
    RtlCopyMemory(data, result, sizeof(result));
    return 0;
}

static int ControlWrite(void *context, const PJ_BYTE *data, PJ_SIZE bytes)
{
    PJ5682S_SPB_WINDOWS *state = context;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR transferred = 0;
    NTSTATUS status;

    if (!state->Opened || !state->Powered || !state->Target ||
        !data || bytes != 4 || !ControlRegister(pj5682s_decode_value(data)))
        return -1;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, (PVOID)data, 4u);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ5682S_SPB_TIMEOUT_MS);
    status = WdfIoTargetSendWriteSynchronously(state->Target, WDF_NO_HANDLE,
                                               &descriptor, NULL, &options, &transferred);
    return NT_SUCCESS(status) && transferred == bytes ? 0 : -1;
}

static NTSTATUS ControlStatus(pj5682s_status status)
{
    switch (status) {
    case PJ5682S_OK: return STATUS_SUCCESS;
    case PJ5682S_WRITE_LOCKED: return STATUS_ACCESS_DENIED;
    case PJ5682S_BAD_ARGUMENT: return STATUS_INVALID_PARAMETER;
    case PJ5682S_INCORRECT_STATE: return STATUS_INVALID_DEVICE_STATE;
    case PJ5682S_WRONG_CHIP: return STATUS_DEVICE_CONFIGURATION_ERROR;
    case PJ5682S_VERIFY_FAILED: return STATUS_DEVICE_PROTOCOL_ERROR;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

static NTSTATUS StopControlLocked(PJ5682S_SPB_WINDOWS *state)
{
    const pj5682s_bus bus = {state, ControlRead, ControlWrite};
    return ControlStatus(pj5682s_control_stop(&state->Control, &bus));
}

NTSTATUS Pj5682sSpbStartControl(PJ5682S_SPB_WINDOWS *state,
                                const pj5682s_control_profile *profile,
                                const pj5682s_analog_program *program)
{
    const pj5682s_bus bus = {state, ControlRead, ControlWrite};
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    PAGED_CODE();
    if (!state || !state->Lock || !profile) return STATUS_INVALID_PARAMETER;
    pj5682s_status valid = pj5682s_validate_analog(program);
    if (valid != PJ5682S_OK) return ControlStatus(valid);
    WdfWaitLockAcquire(state->Lock, NULL);
    if (state->Opened && state->Powered && pj5682s_cold_is_ready(&state->Cold)) {
        status = ControlStatus(pj5682s_control_start_clocked(&state->Control, &bus, profile, ControlDelay));
        if (NT_SUCCESS(status))
            status = ControlStatus(pj5682s_control_analog(&state->Control, &bus, program, ControlDelay));
    }
    WdfWaitLockRelease(state->Lock);
    return status;
}

NTSTATUS Pj5682sSpbStopControl(PJ5682S_SPB_WINDOWS *state)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!state || !state->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(state->Lock, NULL);
    status = StopControlLocked(state);
    WdfWaitLockRelease(state->Lock);
    return status;
}

static int ControlDelay(void *context, unsigned milliseconds)
{
    LARGE_INTEGER delay;
    UNREFERENCED_PARAMETER(context);
    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(milliseconds);
    return NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &delay)) ? 0 : -1;
}

NTSTATUS Pj5682sSpbGetJack(PJ5682S_SPB_WINDOWS *state, pj5682s_jack_query query,
                           pj5682s_jack *jack)
{
    const pj5682s_bus bus = {state, ControlRead, ControlWrite};
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    PAGED_CODE();
    if (!state || !state->Lock || !jack ||
        (unsigned)query > Pj5682sJackReadClassified) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (state->Opened && state->Powered && pj5682s_cold_is_ready(&state->Cold)) {
        pj5682s_status result = query == Pj5682sJackClassify ?
            pj5682s_control_classify(&state->Control, &bus, ControlDelay, jack) :
            pj5682s_control_jack(&state->Control, &bus, query == Pj5682sJackReadClassified, jack);
        status = ControlStatus(result);
    }
    WdfWaitLockRelease(state->Lock);
    return status;
}
#endif
