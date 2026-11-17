/* SPDX-License-Identifier: BSD-3-Clause */
/* Optional KMDF Resource Hub I2C transport. Not hooked into PnP by default.
 * No address, GPIO pin, or bus speed is hard-coded. Caller must validate
 * Pujjo SMBIOS + RTL5682 PnP ID + translated I2C resource BEFORE opening.
 * All entry points require PASSIVE_LEVEL. Windows WDK build NOT verified. */
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
    SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    PAGED_CODE();
    if (!state || !state->Lock || !tx || txLen!=2 || !rx || rxLen!=2) return -1;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (!state->Opened || !state->Target || !state->Powered) {
        WdfWaitLockRelease(state->Lock); return -1;
    }
    SPB_TRANSFER_LIST_INIT(&sequence.List, 2);
    sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, (PVOID)tx, (ULONG)txLen);
    sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, rx, (ULONG)rxLen);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &descriptor, &sequence, sizeof(sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ5682S_SPB_TIMEOUT_MS);
    status = WdfIoTargetSendIoctlSynchronously(
        state->Target, WDF_NO_HANDLE, IOCTL_SPB_EXECUTE_SEQUENCE,
        &descriptor, NULL, &options, &transferred);
    WdfWaitLockRelease(state->Lock);
    /* SPB may return STATUS_SUCCESS after a partial/NACK transfer. */
    return (NT_SUCCESS(status) && transferred == txLen + rxLen) ? 0 : -1;
}

/* This callback is included only for isolated test plumbing; release builds
 * of the codec core ALWAYS return WRITE_LOCKED and never call it. */
static int Pj5682sSpbWrite(void *user, const PJ_BYTE *data, PJ_SIZE length)
{
    PJ5682S_SPB_WINDOWS *state=(PJ5682S_SPB_WINDOWS *)user;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR written=0;
    NTSTATUS status;
    PAGED_CODE();
    if (!state || !state->Lock || !data || length!=4) return -1;
    WdfWaitLockAcquire(state->Lock, NULL);
    if (!state->Opened || !state->Target || !state->Powered) {
        WdfWaitLockRelease(state->Lock); return -1;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, (PVOID)data, (ULONG)length);
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout=WDF_REL_TIMEOUT_IN_MS(PJ5682S_SPB_TIMEOUT_MS);
    status=WdfIoTargetSendWriteSynchronously(
        state->Target, WDF_NO_HANDLE, &descriptor, NULL, &options, &written);
    WdfWaitLockRelease(state->Lock);
    return (NT_SUCCESS(status) && written==length) ? 0 : -1;
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
    bus->write_only=Pj5682sSpbWrite;
    WdfWaitLockRelease(state->Lock);
}
