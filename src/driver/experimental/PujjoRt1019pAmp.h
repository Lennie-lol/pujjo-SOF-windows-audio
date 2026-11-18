/* SPDX-License-Identifier: BSD-3-Clause */
/* RT1019P/RTL1019 amplifier sequencing. Pure, test-only callback layer.
 * No physical GPIO activity is permitted in the production build. */
#ifndef PUJJO_RT1019P_AMP_H
#define PUJJO_RT1019P_AMP_H
#ifdef __cplusplus
extern "C" {
#endif
#define PJ1019_FIRST_ENABLE_DELAY_MS 300u

typedef enum pj1019_status {
    PJ1019_OK = 0, PJ1019_INVALID = 1, PJ1019_UNVERIFIED = 2,
    PJ1019_LOCKED = 3, PJ1019_BAD_STATE = 4, PJ1019_IO_FAILURE = 5
} pj1019_status;
typedef enum pj1019_state {
    PJ1019_UNINITIALIZED = 0, PJ1019_OFF_MUTED = 1,
    PJ1019_ON_MUTED = 2, PJ1019_ON_AUDIO = 3,
    PJ1019_SUSPENDED = 4, PJ1019_FAULT = 5
} pj1019_state;
/* Evidence is NOT filled in by ACPI parsing alone. Every field must be
 * independently verified, notably the output logical polarity and gain. */
typedef struct pj1019_evidence {
    unsigned board_and_acpi_verified;
    unsigned translated_gpio_verified;
    unsigned logical_sdb_polarity_verified;
    unsigned speaker_ssp_and_firmware_verified;
    unsigned dsp_muting_verified;
    unsigned safe_gain_limit_verified;
    unsigned recovery_verified;
    unsigned calibration_procedure_verified;
} pj1019_evidence;
/* Test doubles ONLY; never bind these to kernel or real devices.
 * All callbacks return zero on success. mute(1) silences speaker DSP.
 * set_sdb(1) means abstract logical enable; it is NOT physical pin level. */
typedef struct pj1019_mock_io {
    void *context;
    int (*mute)(void *context, int muted);
    int (*set_sdb)(void *context, int enabled);
    int (*sleep_ms)(void *context, unsigned milliseconds);
} pj1019_mock_io;
typedef struct pj1019_amp {
    pj1019_state state;
    unsigned calibration_required;
} pj1019_amp;

void pj1019_reset(pj1019_amp *amp);
int pj1019_evidence_complete(const pj1019_evidence *e);
pj1019_status pj1019_prepare(pj1019_amp *amp, const pj1019_evidence *e,
                             const pj1019_mock_io *io);
pj1019_status pj1019_power_on_muted(pj1019_amp *amp, const pj1019_mock_io *io);
pj1019_status pj1019_audio_start(pj1019_amp *amp,
                                 const pj1019_mock_io *io, int dsp_stream_ready);
pj1019_status pj1019_stop(pj1019_amp *amp, const pj1019_mock_io *io);
pj1019_status pj1019_suspend(pj1019_amp *amp, const pj1019_mock_io *io);
pj1019_status pj1019_resume(pj1019_amp *amp);
#ifdef __cplusplus
}
#endif
#endif
