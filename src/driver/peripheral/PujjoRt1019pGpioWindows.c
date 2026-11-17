/* SPDX-License-Identifier: BSD-3-Clause */
/* Resource Hub based GPIO I/O backend. SOURCE-ONLY, NEVER INVOKED by probe.
 * Does NOT use ACPI physical GPIO number 75, nor access controller registers.
 * The amp may need a platform-specific logical polarity check before use.
 * WDK compilation and real-hardware behavior are NOT yet verified. */
#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>
#include <gpio.h>
#include "PujjoRt1019pGpioWindows.h"
#define PJ1019_GPIO_TIMEOUT_MS 1000

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
    if (powered && !gpio->Opened) status = STATUS_INVALID_DEVICE_STATE;
    else gpio->Powered = powered;
    WdfWaitLockRelease(gpio->Lock);
    return status;
}

NTSTATUS Pj1019GpioOpen(WDFDEVICE device, ULONG idLow, ULONG idHigh,
                        PJ1019_GPIO_WINDOWS *gpio)
{
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_IO_TARGET_OPEN_PARAMS params;
    WDFIOTARGET target = WDF_NO_HANDLE;
    NTSTATUS status;
    PAGED_CODE();
    if (!device || !gpio || !gpio->Lock || gpio->Owner != device)
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
    gpio->Opened = TRUE;
    gpio->Powered = FALSE;
    WdfWaitLockRelease(gpio->Lock);
    return STATUS_SUCCESS;
}

void Pj1019GpioClose(PJ1019_GPIO_WINDOWS *gpio)
{
    PAGED_CODE();
    if (!gpio || !gpio->Lock) return;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    gpio->Powered = FALSE;
    if (gpio->Target) {
        WdfIoTargetClose(gpio->Target);
        WdfObjectDelete(gpio->Target);
    }
    gpio->Target = WDF_NO_HANDLE;
    gpio->Opened = FALSE;
    WdfWaitLockRelease(gpio->Lock);
}

NTSTATUS Pj1019GpioWrite(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN physicalHigh)
{
    UCHAR value = physicalHigh ? 1u : 0u;
    WDF_MEMORY_DESCRIPTOR input;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    PAGED_CODE();
    if (!gpio || !gpio->Lock) return STATUS_INVALID_DEVICE_STATE;
    WdfWaitLockAcquire(gpio->Lock, NULL);
    if (!gpio->Opened || !gpio->Target || !gpio->Powered) {
        WdfWaitLockRelease(gpio->Lock); return STATUS_INVALID_DEVICE_STATE;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &value, sizeof(value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(PJ1019_GPIO_TIMEOUT_MS);
    status = WdfIoTargetSendIoctlSynchronously(
        gpio->Target, WDF_NO_HANDLE, IOCTL_GPIO_WRITE_PINS,
        &input, NULL, &options, NULL);
    WdfWaitLockRelease(gpio->Lock);
    return status;
}
