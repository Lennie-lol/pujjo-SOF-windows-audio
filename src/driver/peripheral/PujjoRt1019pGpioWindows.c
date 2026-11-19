/* SPDX-License-Identifier: BSD-3-Clause */
/* Resource Hub based GPIO I/O backend. SOURCE-ONLY, NEVER INVOKED by probe.
 * Does NOT use ACPI physical GPIO number 75, nor access controller registers.
 * The amp may need a platform-specific logical polarity check before use.
 * WDK compilation and real-hardware behavior are NOT yet verified. */
/* Enable the inline Resource Hub path helpers before any WDK headers. */
#define RESHUB_USE_HELPER_ROUTINES
#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>
#include <gpio.h>
#include "PujjoRt1019pGpioWindows.h"
#define PJ1019_GPIO_TIMEOUT_MS 1000
#define PJ1019_MIN_CALIBRATION_MS 300u /* rt1015p first-enable delay */
#define PJ1019_MAX_CALIBRATION_MS 2000u /* bounded callback wait */

static NTSTATUS WriteLocked(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN enabled);

NTSTATUS Pj1019GpioInitialize(WDFDEVICE device, PJ1019_GPIO_WINDOWS *gpio)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    PAGED_CODE();
    if (!device || !gpio || gpio->Lock || gpio->Target)
        return STATUS_INVALID_PARAMETER;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &gpio->Lock);
    if (NT_SUCCESS(status)) {
        gpio->Owner = device;
        gpio->Opened = FALSE;
        gpio->Powered = FALSE;
    }
    return status;
}
NTSTATUS Pj1019GpioSetPower(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN powered)
{
    NTSTATUS status = STATUS_SUCCESS;
    PAGED_CODE();
    if (!gpio || !gpio->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    if (powered && !gpio->Opened) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (powered && !gpio->Powered) {
        /* New hardware epoch. Re-establish SDB off before admitting work. */
        gpio->Powered = TRUE;
        gpio->Calibrated = FALSE;
        status = WriteLocked(gpio, FALSE);
        if (NT_SUCCESS(status)) gpio->Faulted = FALSE;
        else gpio->Powered = FALSE;
    } else if (!powered && gpio->Powered) {
        if (!gpio->ShutdownConfirmed) status = WriteLocked(gpio, FALSE);
        gpio->Powered = FALSE;
        gpio->Calibrated = FALSE;
    }
    WdfWaitLockRelease(gpio->Lock);
    return status;
}

NTSTATUS Pj1019GpioOpen(WDFDEVICE device, ULONG idLow, ULONG idHigh,
                        PJ1019_SDB_POLARITY Polarity, PJ1019_GPIO_WINDOWS *gpio)
{
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_IO_TARGET_OPEN_PARAMS params;
    WDFIOTARGET target = WDF_NO_HANDLE;
    NTSTATUS status;
    PAGED_CODE();
    if (!device || !gpio || !gpio->Lock || gpio->Owner != device ||
        (Polarity != Pj1019SdbActiveLow && Polarity != Pj1019SdbActiveHigh))
        return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    if (gpio->Opened || gpio->Target) {
        WdfWaitLockRelease(gpio->Lock);
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, idLow, idHigh);
    if (!NT_SUCCESS(status)) { WdfWaitLockRelease(gpio->Lock); return status; }
    status = WdfIoTargetCreate(device, WDF_NO_OBJECT_ATTRIBUTES, &target);
    if (!NT_SUCCESS(status)) { WdfWaitLockRelease(gpio->Lock); return status; }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&params, &path, GENERIC_WRITE);
    status = WdfIoTargetOpen(target, &params);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(target);
        WdfWaitLockRelease(gpio->Lock);
        return status;
    }
    gpio->Target = target;
    gpio->Polarity = Polarity;
    gpio->ShutdownConfirmed = FALSE;
    gpio->Opened = TRUE;
    gpio->Powered = FALSE;
    WdfWaitLockRelease(gpio->Lock);
    return STATUS_SUCCESS;
}

NTSTATUS Pj1019GpioClose(PJ1019_GPIO_WINDOWS *gpio)
{
    NTSTATUS status = STATUS_SUCCESS;
    PAGED_CODE();
    if (!gpio || !gpio->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    if (gpio->Powered && !gpio->ShutdownConfirmed) status = WriteLocked(gpio, FALSE);
    else if (gpio->Faulted && !gpio->ShutdownConfirmed) status = STATUS_IO_DEVICE_ERROR;
    gpio->Powered = FALSE;
    gpio->Calibrated = FALSE;
    if (gpio->Target) {
        WdfIoTargetClose(gpio->Target);
        WdfObjectDelete(gpio->Target);
    }
    gpio->Target = WDF_NO_HANDLE;
    gpio->Polarity = Pj1019SdbUnverified;
    gpio->Opened = FALSE;
    WdfWaitLockRelease(gpio->Lock);
    return status;
}

static NTSTATUS WriteLocked(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN enabled)
{
    UCHAR value;
    WDF_MEMORY_DESCRIPTOR input;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;

    if (!gpio->Opened || !gpio->Target || !gpio->Powered)
        return STATUS_INVALID_DEVICE_STATE;
    gpio->ShutdownConfirmed = FALSE;
    value = (!!enabled == (gpio->Polarity == Pj1019SdbActiveHigh)) ? 1u : 0u;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &value, sizeof(value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ1019_GPIO_TIMEOUT_MS);
    status = WdfIoTargetSendIoctlSynchronously(
        gpio->Target, WDF_NO_HANDLE, IOCTL_GPIO_WRITE_PINS,
        &input, NULL, &options, NULL);
    if (!NT_SUCCESS(status)) gpio->Faulted = TRUE;
    else gpio->ShutdownConfirmed = !enabled;
    return status;
}

NTSTATUS Pj1019GpioShutdown(PJ1019_GPIO_WINDOWS *gpio)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!gpio || !gpio->Lock) return STATUS_INVALID_PARAMETER;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    status = gpio->Powered && gpio->ShutdownConfirmed ? STATUS_SUCCESS : WriteLocked(gpio, FALSE);
    WdfWaitLockRelease(gpio->Lock);
    return status;
}

NTSTATUS Pj1019GpioStartMuted(PJ1019_GPIO_WINDOWS *gpio, const PJ1019_RUNTIME_PROFILE *profile)
{
    NTSTATUS status;
    LARGE_INTEGER delay;
    PAGED_CODE();
    if (!gpio || !gpio->Lock || !profile) return STATUS_INVALID_PARAMETER;
    if (profile->BoardQualified != TRUE || profile->DspMuted != TRUE ||
        profile->GainLimitApplied != TRUE || profile->CalibrationMs < PJ1019_MIN_CALIBRATION_MS ||
        profile->CalibrationMs > PJ1019_MAX_CALIBRATION_MS)
        return STATUS_ACCESS_DENIED;

    WdfWaitLockAcquire(gpio->Lock, NULL);
    if (!gpio->Powered || !gpio->ShutdownConfirmed || gpio->Faulted) {
        WdfWaitLockRelease(gpio->Lock);
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = WriteLocked(gpio, TRUE);
    if (NT_SUCCESS(status) && !gpio->Calibrated) {
        delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(profile->CalibrationMs);
        status = KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (NT_SUCCESS(status)) gpio->Calibrated = TRUE;
    }
    if (!NT_SUCCESS(status)) {
        NTSTATUS shutdownStatus;
        gpio->Calibrated = FALSE;
        gpio->Faulted = TRUE;
        shutdownStatus = WriteLocked(gpio, FALSE);
        if (!NT_SUCCESS(shutdownStatus)) status = shutdownStatus;
    }
    WdfWaitLockRelease(gpio->Lock);
    return status;
}
