/* SPDX-License-Identifier: BSD-3-Clause */
/* Pujjo topology **inspection only**. No ALSA/WDK integration or hardware I/O. */
#ifndef PUJJO_TOPOLOGY_EVIDENCE_H
#define PUJJO_TOPOLOGY_EVIDENCE_H

#include <stddef.h>
#include <stdint.h>

typedef enum pj_topology_result {
    PJ_TPLG_VALID = 0,
    PJ_TPLG_INVALID_INPUT = -1,
    PJ_TPLG_BAD_HEADER = -2,
    PJ_TPLG_TRUNCATED = -3,
    PJ_TPLG_MISSING_ROUTE = -4
} pj_topology_result;

typedef struct pj_topology_evidence {
    unsigned section_count;
    unsigned has_ssp1_speaker_playback;
    unsigned has_ssp0_headset_playback;
    unsigned has_ssp0_headset_capture;
    unsigned has_ssp2_bt_playback;
    unsigned has_ssp2_bt_capture;
    unsigned has_dmic_capture;
} pj_topology_evidence;

/* These strings only prove that the file contains a plausible candidate graph.
 * They DO NOT establish actual Pujjo wiring or Windows SOF firmware support. */
pj_topology_result pj_inspect_adl_topology(const uint8_t *data, size_t size,
                                           pj_topology_evidence *out);
#endif
