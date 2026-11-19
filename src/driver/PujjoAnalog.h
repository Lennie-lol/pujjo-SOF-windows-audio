/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_ANALOG_H
#define PUJJO_ANALOG_H
#include "Device.h"
#include "peripheral/PujjoPeripheralInterface.h"

typedef struct {
    PUJJO_AUDIO_ROLE Role;
    ULONG VerifiedSsp;
    PUJJO_AUDIO_FORMAT Format;
    LONG MaximumPlaybackGain;
    PUJJO_PERIPHERAL_PROFILE Peripheral;
} PUJJO_ANALOG_PROFILE;

#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
/* Only an independently reviewed board profile may publish an analog PDO.
 * This entry point is called at PASSIVE_LEVEL after DSP boot. */
NTSTATUS SofPublishAnalogChild(PSOF_DEVICE_CONTEXT Ctx, const PUJJO_ANALOG_PROFILE *Profile);
/* Caller holds the audio mutex. */
NTSTATUS SofDisconnectAnalogLocked(PSOF_DEVICE_CONTEXT Ctx);
#endif
#endif
