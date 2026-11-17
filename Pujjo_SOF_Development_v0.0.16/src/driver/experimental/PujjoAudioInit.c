/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoAudioInit.h"
#include <string.h>
static int same(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}
pj_result pj_build_plan(const pj_hardware *hw, pj_plan *out) {
    if (!hw || !out) return PJ_E_INVALID;
    memset(out, 0, sizeof(*out));
    if (!same(hw->manufacturer, "Google") ||
        !(same(hw->board, "Pujjo") || same(hw->board, "Pujjo1e")))
        return PJ_E_WRONG_BOARD;
    if (hw->pci_vendor != 0x8086 || hw->pci_device != 0x54c8 ||
        !same(hw->codec_acpi, "RTL5682") || !same(hw->amp_acpi, "RTL1019"))
        return PJ_E_MISMATCH;
    /* Candidate Linux routes; no hardware readiness is implied. */
    out->speaker_ssp = 1;
    out->headset_ssp = 0;
    out->sample_rate_hz = 48000;
    out->channels = 2;
    out->sample_bits = 24;
    out->initial_amp_delay_ms = 300;
    return PJ_OK;
}
int pj_has_verified_resources(const pj_verification *v) {
    return v && v->firmware_ipc_tested && v->ssp1_routing_tested &&
        v->amp_sdb_gpio_resource_verified && v->dsp_speaker_mute_verified &&
        v->speaker_gain_limiter_verified && v->safe_recovery_available;
}
void pj_audio_reset(pj_audio *a) { if (a) memset(a, 0, sizeof(*a)); }
#ifdef PJ_AUDIO_ENABLE_EXPERIMENTAL_IO
static int ops_valid(const pj_ops *o) {
    return o && o->amp_enable && o->speaker_path_muted &&
        o->speaker_link_configure && o->sleep_ms;
}
/* Best-effort rollback, NOT a proven hardware shutdown implementation. */
static void fail_mute(pj_audio *a, const pj_ops *o) {
    if (o && o->speaker_path_muted) (void)o->speaker_path_muted(o->ctx, 1);
    if (o && o->amp_enable) (void)o->amp_enable(o->ctx, 0);
    if (a) a->state = PJ_STATE_FAULT_MUTED;
}
#endif
pj_result pj_audio_prepare(pj_audio *a, const pj_plan *p,
                           const pj_verification *v, const pj_ops *o) {
    if (!a || !p) return PJ_E_INVALID;
    if (a->state != PJ_STATE_OFF) return PJ_E_STATE;
    if (p->speaker_ssp != 1 || p->headset_ssp != 0 ||
        p->sample_rate_hz != 48000 || p->channels != 2 ||
        p->sample_bits != 24 || p->initial_amp_delay_ms != 300)
        return PJ_E_INVALID;
    if (!pj_has_verified_resources(v)) return PJ_E_UNVERIFIED;
#ifndef PJ_AUDIO_ENABLE_EXPERIMENTAL_IO
    (void)o;
    return PJ_E_NO_BACKEND;
#else
    if (!ops_valid(o)) return PJ_E_NO_BACKEND;
    /* SSP1 speaker: never touch RT5682S headset codec registers here. */
    if (o->amp_enable(o->ctx, 0) != 0 ||
        o->speaker_path_muted(o->ctx, 1) != 0 ||
        o->speaker_link_configure(o->ctx, 1, 48000, 2, 24) != 0) {
        fail_mute(a, o);
        return PJ_E_IO;
    }
    a->plan = *p;
    a->state = PJ_STATE_PREPARED_MUTED;
    return PJ_OK;
#endif
}
pj_result pj_audio_start(pj_audio *a, const pj_ops *o) {
    if (!a) return PJ_E_INVALID;
    if (a->state != PJ_STATE_PREPARED_MUTED) return PJ_E_STATE;
#ifndef PJ_AUDIO_ENABLE_EXPERIMENTAL_IO
    (void)o;
    return PJ_E_NO_BACKEND;
#else
    if (!ops_valid(o)) return PJ_E_NO_BACKEND;
    if (o->amp_enable(o->ctx, 1) != 0) {
        fail_mute(a, o);
        return PJ_E_IO;
    }
    if (!a->amplifier_first_activation_completed) {
        o->sleep_ms(o->ctx, a->plan.initial_amp_delay_ms);
        a->amplifier_first_activation_completed = 1;
    }
    if (o->speaker_path_muted(o->ctx, 0) != 0) {
        fail_mute(a, o);
        return PJ_E_IO;
    }
    a->state = PJ_STATE_PLAYING;
    return PJ_OK;
#endif
}
pj_result pj_audio_stop(pj_audio *a, const pj_ops *o) {
    if (!a) return PJ_E_INVALID;
    if (a->state != PJ_STATE_PLAYING && a->state != PJ_STATE_PREPARED_MUTED)
        return PJ_E_STATE;
#ifndef PJ_AUDIO_ENABLE_EXPERIMENTAL_IO
    (void)o;
    return PJ_E_NO_BACKEND;
#else
    if (!ops_valid(o)) return PJ_E_NO_BACKEND;
    /* DSP speaker mute must precede amplifier shutdown. */
    if (o->speaker_path_muted(o->ctx, 1) != 0 ||
        o->amp_enable(o->ctx, 0) != 0) {
        fail_mute(a, o);
        return PJ_E_IO;
    }
    a->state = PJ_STATE_PREPARED_MUTED;
    return PJ_OK;
#endif
}
