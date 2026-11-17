/* SPDX-License-Identifier: BSD-3-Clause */
/* Pujjo speaker-only coordinator. SOURCE ONLY / no hardware backend. */
#ifndef PUJJO_AUDIO_INIT_H
#define PUJJO_AUDIO_INIT_H
#include <stdint.h>

typedef enum pj_result {
    PJ_OK = 0, PJ_E_INVALID = -1, PJ_E_WRONG_BOARD = -2,
    PJ_E_MISMATCH = -3, PJ_E_UNVERIFIED = -4,
    PJ_E_NO_BACKEND = -5, PJ_E_IO = -6, PJ_E_STATE = -7
} pj_result;
typedef enum pj_state {
    PJ_STATE_OFF = 0, PJ_STATE_PREPARED_MUTED = 1,
    PJ_STATE_PLAYING = 2, PJ_STATE_FAULT_MUTED = 3
} pj_state;
typedef struct pj_hardware {
    const char *manufacturer;
    const char *board;
    uint16_t pci_vendor;
    uint16_t pci_device;
    const char *codec_acpi; /* SSP0 headset: RTL5682; NOT speaker DAC */
    const char *amp_acpi;   /* SSP1 amp: RTL1019; GPIO-controlled */
} pj_hardware;
/* REQUIRE independent on-device validation before any real backend exists.
 * These fields are NOT automatically populated by parsing Linux topology. */
typedef struct pj_verification {
    int firmware_ipc_tested;
    int ssp1_routing_tested;
    int amp_sdb_gpio_resource_verified;
    int dsp_speaker_mute_verified;
    int speaker_gain_limiter_verified;
    int safe_recovery_available;
} pj_verification;
typedef struct pj_plan {
    unsigned speaker_ssp; /* candidate SSP1, Linux reference only */
    unsigned headset_ssp; /* candidate SSP0, separate path */
    unsigned sample_rate_hz;
    unsigned channels;
    unsigned sample_bits; /* valid PCM bits; slot width still unverified */
    unsigned initial_amp_delay_ms;
} pj_plan;
/* The speaker path needs DSP mute and amplifier SDB, not RT5682S registers.
 * This is only a mockable API. No real WDF/ACPI/GPIO implementation is included. */
typedef struct pj_ops {
    int (*amp_enable)(void *ctx, int enabled); /* abstract, actual polarity unknown */
    int (*speaker_path_muted)(void *ctx, int muted); /* mute the SSP1 DSP path */
    int (*speaker_link_configure)(void *ctx, unsigned ssp, unsigned hz,
                                  unsigned channels, unsigned valid_bits);
    void (*sleep_ms)(void *ctx, unsigned ms);
    void *ctx;
} pj_ops;
typedef struct pj_audio {
    pj_state state;
    pj_plan plan;
    int amplifier_first_activation_completed;
} pj_audio;
pj_result pj_build_plan(const pj_hardware *hw, pj_plan *out);
int pj_has_verified_resources(const pj_verification *verification);
void pj_audio_reset(pj_audio *audio);
pj_result pj_audio_prepare(pj_audio *audio, const pj_plan *plan,
                           const pj_verification *verification, const pj_ops *ops);
pj_result pj_audio_start(pj_audio *audio, const pj_ops *ops);
pj_result pj_audio_stop(pj_audio *audio, const pj_ops *ops);
#endif
