/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoRt1019pAmp.h"
#include <string.h>
/* Deliberately incompatible with an accidental shipping kernel build. */
#if defined(PJ1019_TEST_ONLY_CALLBACKS) && !defined(PJ1019_TEST_BUILD)
#error PJ1019_TEST_ONLY_CALLBACKS requires PJ1019_TEST_BUILD (unit tests only)
#endif
void pj1019_reset(pj1019_amp *a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->state = PJ1019_UNINITIALIZED;
    a->calibration_required = 1u;
}
int pj1019_evidence_complete(const pj1019_evidence *e) {
    return e && e->board_and_acpi_verified &&
        e->translated_gpio_verified && e->logical_sdb_polarity_verified &&
        e->speaker_ssp_and_firmware_verified && e->dsp_muting_verified &&
        e->safe_gain_limit_verified && e->recovery_verified;
}
#ifdef PJ1019_TEST_ONLY_CALLBACKS
static int io_valid(const pj1019_mock_io *io) {
    return io && io->mute && io->set_sdb && io->sleep_ms;
}
static pj1019_status fault(pj1019_amp *a, const pj1019_mock_io *io) {
    /* Best effort: keep attempting to turn off SDB even if mute fails. */
    if (io && io->mute) (void)io->mute(io->context, 1);
    if (io && io->set_sdb) (void)io->set_sdb(io->context, 0);
    a->state = PJ1019_FAULT;
    a->calibration_required = 1u;
    return PJ1019_IO_FAILURE;
}
#endif
pj1019_status pj1019_prepare(pj1019_amp *a, const pj1019_evidence *e,
                             const pj1019_mock_io *io) {
    if (!a) return PJ1019_INVALID;
    if (a->state != PJ1019_UNINITIALIZED) return PJ1019_BAD_STATE;
    if (!pj1019_evidence_complete(e)) return PJ1019_UNVERIFIED;
#ifndef PJ1019_TEST_ONLY_CALLBACKS
    (void)io;
    return PJ1019_LOCKED;
#else
    if (!io_valid(io)) return PJ1019_INVALID;
    /* Mute before touching SDB. Power-on is a separate call. */
    if (io->mute(io->context, 1) != 0 ||
        io->set_sdb(io->context, 0) != 0) return fault(a, io);
    a->state = PJ1019_OFF_MUTED;
    return PJ1019_OK;
#endif
}
pj1019_status pj1019_power_on_muted(pj1019_amp *a, const pj1019_mock_io *io) {
    if (!a) return PJ1019_INVALID;
    if (a->state != PJ1019_OFF_MUTED) return PJ1019_BAD_STATE;
#ifndef PJ1019_TEST_ONLY_CALLBACKS
    (void)io;
    return PJ1019_LOCKED;
#else
    if (!io_valid(io)) return PJ1019_INVALID;
    if (io->mute(io->context, 1) != 0 ||
        io->set_sdb(io->context, 1) != 0) return fault(a, io);
    if (a->calibration_required) {
        if (io->sleep_ms(io->context, PJ1019_FIRST_ENABLE_DELAY_MS) != 0)
            return fault(a, io);
        a->calibration_required = 0u;
    }
    /* Never unmute here. The driver must prove DSP streaming is ready first. */
    a->state = PJ1019_ON_MUTED;
    return PJ1019_OK;
#endif
}
pj1019_status pj1019_audio_start(pj1019_amp *a,
                                 const pj1019_mock_io *io, int stream_ready) {
    if (!a) return PJ1019_INVALID;
    if (a->state != PJ1019_ON_MUTED) return PJ1019_BAD_STATE;
    if (!stream_ready) return PJ1019_UNVERIFIED;
#ifndef PJ1019_TEST_ONLY_CALLBACKS
    (void)io;
    return PJ1019_LOCKED;
#else
    if (!io_valid(io)) return PJ1019_INVALID;
    if (io->mute(io->context, 0) != 0) return fault(a, io);
    a->state = PJ1019_ON_AUDIO;
    return PJ1019_OK;
#endif
}
pj1019_status pj1019_stop(pj1019_amp *a, const pj1019_mock_io *io) {
    if (!a) return PJ1019_INVALID;
    if (a->state != PJ1019_ON_AUDIO && a->state != PJ1019_ON_MUTED &&
        a->state != PJ1019_OFF_MUTED && a->state != PJ1019_FAULT)
        return PJ1019_BAD_STATE;
#ifndef PJ1019_TEST_ONLY_CALLBACKS
    (void)io;
    return PJ1019_LOCKED;
#else
    int mute_ok, gpio_ok;
    if (!io_valid(io)) return PJ1019_INVALID;
    /* Always attempt BOTH operations; if mute fails, SDB must still be shut. */
    mute_ok = io->mute(io->context, 1);
    gpio_ok = io->set_sdb(io->context, 0);
    if (mute_ok || gpio_ok) return fault(a, io);
    /* A successful emergency shutdown does not clear a latched fault. */
    if (a->state == PJ1019_FAULT) return PJ1019_IO_FAILURE;
    a->state = PJ1019_OFF_MUTED;
    return PJ1019_OK;
#endif
}
pj1019_status pj1019_suspend(pj1019_amp *a, const pj1019_mock_io *io) {
    pj1019_status status;
    if (!a) return PJ1019_INVALID;
    if (a->state == PJ1019_SUSPENDED) return PJ1019_OK;
    if (a->state == PJ1019_UNINITIALIZED) return PJ1019_BAD_STATE;
    status = pj1019_stop(a, io);
    if (status != PJ1019_OK) return status;
    a->state = PJ1019_SUSPENDED;
    a->calibration_required = 1u;
    return PJ1019_OK;
}
pj1019_status pj1019_resume(pj1019_amp *a) {
    if (!a) return PJ1019_INVALID;
    if (a->state != PJ1019_SUSPENDED) return PJ1019_BAD_STATE;
    /* Never automatically enable SDB when resuming. */
    a->state = PJ1019_OFF_MUTED;
    return PJ1019_OK;
}
