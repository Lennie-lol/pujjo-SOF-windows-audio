/* SPDX-License-Identifier: BSD-3-Clause */
/* Optional Windows KMDF SPB transport; not invoked by PnP probe project. */
#ifndef PUJJO_RT5682S_SPB_WINDOWS_H
#define PUJJO_RT5682S_SPB_WINDOWS_H
#include <ntddk.h>
#include <wdf.h>
#include "PujjoRt5682sCodec.h"
#ifdef PJ5682S_ENABLE_CONTROL
#include "PujjoRt5682sControl.h"
#endif
typedef struct _PJ5682S_SPB_WINDOWS {
    WDFWAITLOCK Lock; /* device-parented; survives resource rebalance */
    WDFDEVICE Owner;
    WDFIOTARGET Target;
    BOOLEAN Opened;
    BOOLEAN Powered;
#ifdef PJ5682S_ENABLE_CONTROL
    pj5682s_control Control;
#endif
} PJ5682S_SPB_WINDOWS;
/* Initialize exactly once during DeviceAdd, before publishing this state.
 * All later operations require PASSIVE_LEVEL; context must outlive callbacks.
 * Close drains the bounded synchronous call under Lock. It does not delete Lock.
 * Parent-device deletion owns final lock disposal. No caller may zero a live state. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj5682sSpbInitialize(WDFDEVICE device, PJ5682S_SPB_WINDOWS *state);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj5682sSpbSetPower(PJ5682S_SPB_WINDOWS *state, BOOLEAN powered);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Pj5682sSpbOpen(WDFDEVICE device, ULONG idLow, ULONG idHigh,
                        PJ5682S_SPB_WINDOWS *state);
NTSTATUS Pj5682sSpbClose(PJ5682S_SPB_WINDOWS *state);
void Pj5682sSpbBindCallbacks(PJ5682S_SPB_WINDOWS *state, pj5682s_bus *bus);
#ifdef PJ5682S_ENABLE_CONTROL
/* Kernel audio owner only. Call while the DSP and analog path are muted. */
NTSTATUS Pj5682sSpbStartControl(PJ5682S_SPB_WINDOWS *state,
                                const pj5682s_control_profile *profile);
NTSTATUS Pj5682sSpbStopControl(PJ5682S_SPB_WINDOWS *state);
NTSTATUS Pj5682sSpbGetJack(PJ5682S_SPB_WINDOWS *state, BOOLEAN detectionCompleted,
                           pj5682s_jack *jack);
#endif
#endif
