/* SPDX-License-Identifier: BSD-3-Clause */
/* Windows KMDF GPIO transport prototype, not connected to PnP or playback. */
#ifndef PUJJO_RT1019P_GPIO_WINDOWS_H
#define PUJJO_RT1019P_GPIO_WINDOWS_H
#include <ntddk.h>
#include <wdf.h>
typedef enum _PJ1019_SDB_POLARITY {
    Pj1019SdbUnverified = 0,
    Pj1019SdbActiveLow,
    Pj1019SdbActiveHigh
} PJ1019_SDB_POLARITY;

typedef struct _PJ1019_RUNTIME_PROFILE {
    BOOLEAN BoardQualified;
    BOOLEAN DspMuted;
    BOOLEAN GainLimitApplied;
    ULONG CalibrationMs;
} PJ1019_RUNTIME_PROFILE;

typedef struct _PJ1019_GPIO_WINDOWS {
    WDFWAITLOCK Lock; /* device-parented; survives resource rebalance */
    WDFDEVICE Owner;
    WDFIOTARGET Target;
    BOOLEAN Opened;
    BOOLEAN Powered;
    BOOLEAN ShutdownConfirmed;
    BOOLEAN Calibrated;
    BOOLEAN Faulted;
    PJ1019_SDB_POLARITY Polarity;
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
                        PJ1019_SDB_POLARITY Polarity, PJ1019_GPIO_WINDOWS *gpio);
NTSTATUS Pj1019GpioClose(PJ1019_GPIO_WINDOWS *gpio);
/* The DSP owner holds mute before calling StartMuted or Shutdown. Only
 * StartMuted may assert SDB. Shutdown also works after a latched fault. */
NTSTATUS Pj1019GpioStartMuted(PJ1019_GPIO_WINDOWS *gpio, const PJ1019_RUNTIME_PROFILE *profile);
NTSTATUS Pj1019GpioShutdown(PJ1019_GPIO_WINDOWS *gpio);
#endif
