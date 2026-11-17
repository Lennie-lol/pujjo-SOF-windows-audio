/* SPDX-License-Identifier: BSD-3-Clause */
/* Pujjo SSP1 speaker bring-up contract (PORTABLE MODEL ONLY).
 * Does not contain a kernel I/O backend or permit installation.
 * Caller must serialize every API and must not re-enter from a callback.
 * This model provides no kernel synchronization/backend.
 * Hardware actions are compiled out except in explicit host-only mock tests. */
#ifndef PUJJO_BRINGUP_COORDINATOR_H
#define PUJJO_BRINGUP_COORDINATOR_H
#include "PujjoAudioInit.h"
#include "PujjoRt1019pAmp.h"
#include "PujjoSofFirmwarePolicy.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PJ_BRINGUP_OK = 0, PJ_BRINGUP_BAD_ARG = -1,
    PJ_BRINGUP_NOT_VERIFIED = -2, PJ_BRINGUP_WRONG_STATE = -3,
    PJ_BRINGUP_LOCKED = -4, PJ_BRINGUP_IO_ERROR = -5,
    PJ_BRINGUP_FAULT_LATCHED = -6
} pj_bringup_result;
typedef enum {
    PJ_BRINGUP_RESET = 0, PJ_BRINGUP_PREPARED_MUTED,
    PJ_BRINGUP_AMP_ON_MUTED, PJ_BRINGUP_STREAMING,
    PJ_BRINGUP_SUSPENDED, PJ_BRINGUP_FAULT
} pj_bringup_state;
/* These are independent field measurements/reviews, NEVER inferred from
 * firmware filenames, ACPI node presence, or successful mock tests. */
typedef struct {
    pj_hardware hardware;
    pj_verification speaker;
    PJ_SOF_FW_EVIDENCE firmware;
    pj1019_evidence amplifier;
    unsigned compatible_ipc4_image_and_windows_loader_verified;
    unsigned windows_ssp1_graph_and_clock_verified;
    unsigned physical_sdb_polarity_and_speaker_limits_measured;
    unsigned windows_pnp_power_and_teardown_verified;
} pj_bringup_evidence;
/* TEST DOUBLE ONLY. No callback may access a WDF device or real audio HW.
 * Return 0 on success, nonzero on failure. stream_ready returns 1 only when
 * an already-running mock audio pipeline can accept samples. */
typedef struct {
    void *context;
    int (*dsp_speaker_mute)(void *);
    int (*dsp_speaker_unmute)(void *);
    int (*amp_disable)(void *);
    int (*dsp_fw_ready)(void *);
    int (*ssp1_prepare)(void *);
    int (*gain_limiter_ready)(void *);
    int (*amp_enable)(void *);
    int (*delay_ms)(void *, unsigned);
    int (*stream_ready)(void *);
} pj_bringup_mock_ops;
typedef struct {
    pj_bringup_state state;
    unsigned first_activation_required;
    unsigned ready_epoch;
} pj_bringup;
void pj_bringup_reset(pj_bringup *b);
int pj_bringup_evidence_complete(const pj_bringup_evidence *e);
pj_bringup_result pj_bringup_prepare(pj_bringup *b,
        const pj_bringup_evidence *e, const pj_bringup_mock_ops *ops);
pj_bringup_result pj_bringup_amp_on_muted(pj_bringup *b,
        const pj_bringup_mock_ops *ops);
pj_bringup_result pj_bringup_start_stream(pj_bringup *b,
        const pj_bringup_mock_ops *ops);
pj_bringup_result pj_bringup_stop(pj_bringup *b,
        const pj_bringup_mock_ops *ops);
pj_bringup_result pj_bringup_suspend(pj_bringup *b,
        const pj_bringup_mock_ops *ops);
/* Resume never powers a speaker automatically; reacquire fresh evidence
 * and call prepare only after a new explicit reset by the host test. */
pj_bringup_result pj_bringup_resume_to_reset(pj_bringup *b);
#ifdef __cplusplus
}
#endif
#endif
