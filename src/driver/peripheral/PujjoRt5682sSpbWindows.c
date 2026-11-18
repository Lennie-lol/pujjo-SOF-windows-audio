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
    else state->Powered = powered;
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
    WdfWaitLockRelease(state->Lock);
    return STATUS_SUCCESS;
}
void Pj5682sSpbClose(PJ5682S_SPB_WINDOWS *state)
{
    PAGED_CODE();
    if (!state || !state->Lock) return;
    WdfWaitLockAcquire(state->Lock, NULL);
    state->Powered = FALSE;
    if (state->Target) {
        WdfIoTargetClose(state->Target);
        WdfObjectDelete(state->Target);
    }
    state->Target=WDF_NO_HANDLE;
    state->Opened=FALSE;
    WdfWaitLockRelease(state->Lock);
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
