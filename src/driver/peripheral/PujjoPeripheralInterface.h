/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_PERIPHERAL_INTERFACE_H
#define PUJJO_PERIPHERAL_INTERFACE_H
#include <ntddk.h>
#include <guiddef.h>
#include "PujjoRt5682sControl.h"

DEFINE_GUID(GUID_PUJJO_PERIPHERAL_CONTROL, 0x0a21dc90, 0xd8a2, 0x47b6,
            0x89, 0x55, 0x83, 0x46, 0x94, 0x7b, 0xeb, 0x2f);
DEFINE_GUID(GUID_DEVINTERFACE_PUJJO_CODEC_CONTROL, 0x4fda23ae, 0xac3a, 0x4ea6,
            0xb0, 0x20, 0x37, 0xc9, 0xd3, 0xf2, 0x60, 0x6d);
DEFINE_GUID(GUID_DEVINTERFACE_PUJJO_AMP_CONTROL, 0x9fcf5a91, 0x9041, 0x49a6,
            0xb3, 0xe7, 0x70, 0x0d, 0x39, 0x6a, 0x77, 0xd4);

#define PUJJO_PERIPHERAL_CONTROL_VERSION 3
#define PUJJO_CONTROL_CODEC 1u
#define PUJJO_CONTROL_AMP 2u
#define PujjoAnalogSdbActiveLow 1u
#define PujjoAnalogSdbActiveHigh 2u

typedef struct {
    unsigned BoardQualified;
    unsigned GainLimitApplied;
    unsigned DspMuted;
    unsigned CalibrationMs;
    /* 0 is unknown. 1 means active low, 2 means active high. */
    unsigned SdbPolarity;
    pj5682s_control_profile Codec;
    pj5682s_analog_program CodecAnalog;
} PUJJO_PERIPHERAL_PROFILE;

/* In-kernel PnP query interface. IsActive uses an atomic snapshot and
 * can run at DISPATCH_LEVEL. Prepare, Shutdown and ReadJack run at
 * PASSIVE_LEVEL and serialize with provider suspend and removal. The
 * caller retains the interface while using its route and provides a unique
 * non-null cookie for its lease. The provider compares but never dereferences it. */
typedef struct {
    INTERFACE Header;
    ULONG Kind;
    NTSTATUS (*Prepare)(PVOID Context, PVOID Owner, const PUJJO_PERIPHERAL_PROFILE *Profile);
    NTSTATUS (*Shutdown)(PVOID Context, PVOID Owner);
    BOOLEAN (*IsActive)(PVOID Context, PVOID Owner);
    NTSTATUS (*ReadJack)(PVOID Context, PVOID Owner, BOOLEAN ClassificationReady, pj5682s_jack *Jack);
} PUJJO_PERIPHERAL_INTERFACE;
#endif
