/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Pujjo Linux-topology reference (not an operational audio route).
 *
 * Source: WeirdTreeThing/chromebook-linux-audio, Linux ALSA SOF topology
 * blobs/adl/sof-adl-rt1019-rt5682.tplg and setup-audio.
 *
 * These mappings are clues for Windows porting, NOT validated Windows IPC4
 * settings. Do NOT use them to enable power, clocks or speaker playback.
 */
#ifndef PUJJO_LINUX_TOPOLOGY_H
#define PUJJO_LINUX_TOPOLOGY_H

#define PUJJO_AUDIO_PCI_VENDOR_ID         0x8086U
#define PUJJO_AUDIO_PCI_DEVICE_ID         0x54C8U

/* Observed on Pujjo rev3; Linux setup-audio names the variants as below. */
#define PUJJO_AUDIO_ACPI_CODEC            "RTL5682" /* Linux: rt5682s */
#define PUJJO_AUDIO_ACPI_AMPLIFIER        "RTL1019" /* Linux: rt1019p -> rt1015p.c */

/* Reference Linux .tplg graph, NOT currently enabled by Windows driver. */
#define PUJJO_LINUX_SSP_SPEAKERS          1U /* PCM0P -> SSP1.OUT */
#define PUJJO_LINUX_SSP_HEADSET           0U /* PCM1P/C -> SSP0.OUT/IN */
#define PUJJO_LINUX_SSP_BT_LINK           2U /* PCM7P/C -> SSP2.OUT/IN */

#define PUJJO_LINUX_PCM_SPEAKERS          0U
#define PUJJO_LINUX_PCM_HEADSET           1U
#define PUJJO_LINUX_PCM_BT_LINK           7U

/* Re-validation required: fw IPC compatibility, TDM slots, clocks, volume. */
#define PUJJO_WINDOWS_SPEAKER_PATH_READY  0U

#endif
