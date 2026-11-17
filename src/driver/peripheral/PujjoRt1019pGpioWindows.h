/* SPDX-License-Identifier: BSD-3-Clause */
/* Windows KMDF GPIO transport prototype, not connected to PnP or playback. */
#ifndef PUJJO_RT1019P_GPIO_WINDOWS_H
#define PUJJO_RT1019P_GPIO_WINDOWS_H
#include <ntddk.h>
#include <wdf.h>
typedef struct _PJ1019_GPIO_WINDOWS {
    WDFWAITLOCK Lock; /* device-parented; survives resource rebalance */
    WDFDEVICE Owner;
    WDFIOTARGET Target;
    BOOLEAN Opened;
    BOOLEAN Powered;
} PJ1019_GPIO_WINDOWS;
/* Caller must validate: Google Pujjo SMBIOS, RTL1019 PnP hardware ID,
 * exactly one translated GPIO I/O connection and expected pin grouping. */
/* Initialize exactly once during DeviceAdd, before publishing this state.
 * All later operations require PASSIVE_LEVEL; context must outlive callbacks.
 * Close drains the bounded synchronous call under Lock. It does not delete Lock.
 * Parent-device deletion owns final lock disposal. No caller may zero a live state. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj1019GpioInitialize(WDFDEVICE device, PJ1019_GPIO_WINDOWS *gpio);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj1019GpioSetPower(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN powered);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj1019GpioOpen(WDFDEVICE device, ULONG idLow, ULONG idHigh,
                        PJ1019_GPIO_WINDOWS *gpio);
void Pj1019GpioClose(PJ1019_GPIO_WINDOWS *gpio);
/* Raw physical level; caller MUST resolve measured polarity. No enable call exists
 * from the shipping PnP probe; avoid using until polarity/safety verified. */
NTSTATUS Pj1019GpioWrite(PJ1019_GPIO_WINDOWS *gpio, BOOLEAN physicalHigh);
#endif
