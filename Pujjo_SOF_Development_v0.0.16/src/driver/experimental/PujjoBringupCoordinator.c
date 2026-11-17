/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoBringupCoordinator.h"
#include <string.h>
/* Prevent accidental enabling in a Windows kernel driver project. */
#if defined(PJ_BRINGUP_TEST_CALLBACKS) && (!defined(PJ_BRINGUP_HOST_TEST) || defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDF_H_))
#error "PJ_BRINGUP_TEST_CALLBACKS is permitted in user-mode host tests ONLY"
#endif
void pj_bringup_reset(pj_bringup *b)
{
    if (!b) return;
    (void)memset(b, 0, sizeof(*b));
    b->state = PJ_BRINGUP_RESET;
    b->first_activation_required = 1u;
}
int pj_bringup_evidence_complete(const pj_bringup_evidence *e)
{
    pj_plan plan;
    if (!e || pj_build_plan(&e->hardware, &plan) != PJ_OK) return 0;
    /* Literal 1, not any arbitrary non-zero value. */
    return pj_has_verified_resources(&e->speaker) &&
        e->speaker.firmware_ipc_tested == 1 &&
        e->speaker.ssp1_routing_tested == 1 &&
        e->speaker.amp_sdb_gpio_resource_verified == 1 &&
        e->speaker.dsp_speaker_mute_verified == 1 &&
        e->speaker.speaker_gain_limiter_verified == 1 &&
        e->speaker.safe_recovery_available == 1 &&
        pj_sof_evidence_complete(&e->firmware) &&
        pj1019_evidence_complete(&e->amplifier) &&
        e->amplifier.board_and_acpi_verified == 1u &&
        e->amplifier.translated_gpio_verified == 1u &&
        e->amplifier.logical_sdb_polarity_verified == 1u &&
        e->amplifier.speaker_ssp_and_firmware_verified == 1u &&
        e->amplifier.dsp_muting_verified == 1u &&
        e->amplifier.safe_gain_limit_verified == 1u &&
        e->amplifier.recovery_verified == 1u &&
        e->compatible_ipc4_image_and_windows_loader_verified == 1u &&
        e->windows_ssp1_graph_and_clock_verified == 1u &&
        e->physical_sdb_polarity_and_speaker_limits_measured == 1u &&
        e->windows_pnp_power_and_teardown_verified == 1u;
}
#ifdef PJ_BRINGUP_TEST_CALLBACKS
static int callbacks_valid(const pj_bringup_mock_ops *o)
{
    return o && o->dsp_speaker_mute && o->dsp_speaker_unmute &&
        o->amp_disable && o->dsp_fw_ready && o->ssp1_prepare &&
        o->gain_limiter_ready && o->amp_enable && o->delay_ms &&
        o->stream_ready;
}
static pj_bringup_result latch_fault(pj_bringup *b,
        const pj_bringup_mock_ops *o)
{
    /* Make both calls even if the first fails. Never clear a fault by stop. */
    if (o && o->dsp_speaker_mute)
        (void)o->dsp_speaker_mute(o->context);
    if (o && o->amp_disable)
        (void)o->amp_disable(o->context);
    b->state = PJ_BRINGUP_FAULT;
    b->first_activation_required = 1u;
    b->ready_epoch = 0u;
    return PJ_BRINGUP_IO_ERROR;
}
#endif
pj_bringup_result pj_bringup_prepare(pj_bringup *b,
        const pj_bringup_evidence *e, const pj_bringup_mock_ops *ops)
{
    if (!b || !e) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) return PJ_BRINGUP_FAULT_LATCHED;
    if (b->state != PJ_BRINGUP_RESET) return PJ_BRINGUP_WRONG_STATE;
    if (!pj_bringup_evidence_complete(e)) return PJ_BRINGUP_NOT_VERIFIED;
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    (void)ops;
    return PJ_BRINGUP_LOCKED;
#else
    if (!callbacks_valid(ops)) return PJ_BRINGUP_BAD_ARG;
    /* First mute and shut down the amp BEFORE interrogating the mock DSP. */
    if (ops->dsp_speaker_mute(ops->context) != 0 ||
        ops->amp_disable(ops->context) != 0 ||
        ops->dsp_fw_ready(ops->context) != 0 ||
        ops->ssp1_prepare(ops->context) != 0 ||
        ops->gain_limiter_ready(ops->context) != 0)
        return latch_fault(b, ops);
    b->state = PJ_BRINGUP_PREPARED_MUTED;
    b->ready_epoch = 1u;
    return PJ_BRINGUP_OK;
#endif
}
pj_bringup_result pj_bringup_amp_on_muted(pj_bringup *b,
        const pj_bringup_mock_ops *ops)
{
    if (!b) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) return PJ_BRINGUP_FAULT_LATCHED;
    if (b->state != PJ_BRINGUP_PREPARED_MUTED || !b->ready_epoch)
        return PJ_BRINGUP_WRONG_STATE;
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    (void)ops;
    return PJ_BRINGUP_LOCKED;
#else
    if (!callbacks_valid(ops)) return PJ_BRINGUP_BAD_ARG;
    if (ops->dsp_speaker_mute(ops->context) != 0 ||
        ops->amp_enable(ops->context) != 0)
        return latch_fault(b, ops);
    if (b->first_activation_required) {
        if (ops->delay_ms(ops->context, PJ1019_FIRST_ENABLE_DELAY_MS) != 0)
            return latch_fault(b, ops);
        b->first_activation_required = 0u;
    }
    b->state = PJ_BRINGUP_AMP_ON_MUTED;
    return PJ_BRINGUP_OK;
#endif
}
pj_bringup_result pj_bringup_start_stream(pj_bringup *b,
        const pj_bringup_mock_ops *ops)
{
    if (!b) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) return PJ_BRINGUP_FAULT_LATCHED;
    if (b->state != PJ_BRINGUP_AMP_ON_MUTED || !b->ready_epoch)
        return PJ_BRINGUP_WRONG_STATE;
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    (void)ops;
    return PJ_BRINGUP_LOCKED;
#else
    if (!callbacks_valid(ops)) return PJ_BRINGUP_BAD_ARG;
    /* Stream readiness is queried immediately before unmuting. */
    {
        int ready = ops->stream_ready(ops->context);
        if (ready < 0) return latch_fault(b, ops);
        if (ready != 1) return PJ_BRINGUP_NOT_VERIFIED;
    }
    if (ops->dsp_speaker_unmute(ops->context) != 0)
        return latch_fault(b, ops);
    b->state = PJ_BRINGUP_STREAMING;
    return PJ_BRINGUP_OK;
#endif
}
pj_bringup_result pj_bringup_stop(pj_bringup *b,
        const pj_bringup_mock_ops *ops)
{
    if (!b) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) {
#ifdef PJ_BRINGUP_TEST_CALLBACKS
        /* Fault must not prevent another emergency off attempt. */
        (void)latch_fault(b, ops);
#else
        (void)ops;
#endif
        return PJ_BRINGUP_FAULT_LATCHED;
    }
    if (b->state != PJ_BRINGUP_PREPARED_MUTED &&
        b->state != PJ_BRINGUP_AMP_ON_MUTED &&
        b->state != PJ_BRINGUP_STREAMING)
        return PJ_BRINGUP_WRONG_STATE;
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    (void)ops;
    return PJ_BRINGUP_LOCKED;
#else
    int mute_result, amp_result;
    if (!callbacks_valid(ops)) return PJ_BRINGUP_BAD_ARG;
    mute_result = ops->dsp_speaker_mute(ops->context);
    amp_result = ops->amp_disable(ops->context);
    if (mute_result || amp_result) return latch_fault(b, ops);
    b->state = PJ_BRINGUP_PREPARED_MUTED;
    return PJ_BRINGUP_OK;
#endif
}
pj_bringup_result pj_bringup_suspend(pj_bringup *b,
        const pj_bringup_mock_ops *ops)
{
    pj_bringup_result result;
    if (!b) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) return PJ_BRINGUP_FAULT_LATCHED;
    if (b->state != PJ_BRINGUP_PREPARED_MUTED &&
        b->state != PJ_BRINGUP_AMP_ON_MUTED &&
        b->state != PJ_BRINGUP_STREAMING)
        return PJ_BRINGUP_WRONG_STATE;
    result = pj_bringup_stop(b, ops);
    if (result != PJ_BRINGUP_OK) return result;
    b->state = PJ_BRINGUP_SUSPENDED;
    b->ready_epoch = 0u;  /* Never reuse DSP/link evidence across D3. */
    b->first_activation_required = 1u;
    return PJ_BRINGUP_OK;
}
pj_bringup_result pj_bringup_resume_to_reset(pj_bringup *b)
{
    if (!b) return PJ_BRINGUP_BAD_ARG;
    if (b->state == PJ_BRINGUP_FAULT) return PJ_BRINGUP_FAULT_LATCHED;
    if (b->state != PJ_BRINGUP_SUSPENDED) return PJ_BRINGUP_WRONG_STATE;
    pj_bringup_reset(b);
    return PJ_BRINGUP_OK;
}
