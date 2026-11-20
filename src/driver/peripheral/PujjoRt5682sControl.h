/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_RT5682S_CONTROL_H
#define PUJJO_RT5682S_CONTROL_H
#include "../experimental/PujjoHeadsetClock.h"
#include "../experimental/PujjoRt5682sCodec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PJ5682S_ROUTE_DAC 1u
#define PJ5682S_ROUTE_ADC 2u
#define PJ5682S_MAX_ANALOG_STEPS 24u
#define PJ5682S_MAX_START_WRITES 16u
#define PJ5682S_MAX_SAVED_REGISTERS (PJ5682S_MAX_START_WRITES + PJ5682S_MAX_ANALOG_STEPS + 1u)

typedef struct {
    PJ_WORD reg, mask, value;
    PJ_WORD settle_ms, restore_ms;
} pj5682s_analog_step;

typedef struct {
    unsigned count;
    pj5682s_analog_step steps[PJ5682S_MAX_ANALOG_STEPS];
} pj5682s_analog_program;

typedef int (*pj5682s_wait)(void *context, unsigned ms);

/* Supplied by the kernel board owner. No Pujjo production profile is
 * implied by a successful SMBIOS, codec ID or NHLT match. */
typedef struct {
    unsigned board_qualified;
    unsigned analog_path_prepared_muted;
    unsigned dsp_muted;
    unsigned jd1_qualified;
    unsigned mclk_hz, bclk_hz, lrclk_hz, slot_bits, inverted_bclk;
    unsigned routes;
    /* Separately reviewed PLLB startup delay; 0 for direct MCLK. */
    unsigned pll2_settle_ms;
    unsigned pll2_qualified;
} pj5682s_control_profile;

typedef struct {
    PJ_WORD reg, mask, original, restore_ms;
} pj5682s_saved_register;

typedef struct {
    pj5682s_saved_register saved[PJ5682S_MAX_SAVED_REGISTERS];
    unsigned count;
    unsigned active;
    unsigned faulted;
    unsigned jd1_qualified;
    pj5682s_wait wait;
} pj5682s_control;

typedef enum {
    PJ5682S_JACK_ABSENT,
    PJ5682S_JACK_PRESENT_UNCLASSIFIED,
    PJ5682S_JACK_HEADPHONE,
    PJ5682S_JACK_HEADSET
} pj5682s_jack;

/* Board-approved analog power/routing operations are limited to stable,
 * recognized fields. This program never accepts reset, clock, IRQ or OTP writes. */
PJ_WORD pj5682s_analog_mask(PJ_WORD reg);
pj5682s_status pj5682s_validate_analog(const pj5682s_analog_program *program);
pj5682s_status pj5682s_control_analog(pj5682s_control *state, const pj5682s_bus *bus,
    const pj5682s_analog_program *program, pj5682s_wait wait);


/* Cold-start owns the entire powered codec, not a stream lease. The
 * board owner must serialize it with removal, power and route operations.
 * Qualification flags must come from independently reviewed board evidence.
 * No Windows caller is enabled by this API. State is zeroed only after a
 * confirmed new power epoch; never clear a fault to retry on a live codec. */
typedef enum {
    PJ5682S_COLD_IDLE = 0,
    PJ5682S_COLD_SETTLING,
    PJ5682S_COLD_RESET,
    PJ5682S_COLD_PATCH,
    PJ5682S_COLD_PREPARE,
    PJ5682S_COLD_CALIBRATING,
    PJ5682S_COLD_RESTORE,
    PJ5682S_COLD_READY,
    PJ5682S_COLD_FAILED
} pj5682s_cold_phase;

typedef struct {
    unsigned board_qualified, exclusive_owner, rails_ready, outputs_isolated;
} pj5682s_cold_profile;

typedef struct {
    pj5682s_cold_phase phase, failed_at;
    unsigned polls, busy_seen, reset_attempted;
    pj5682s_status recovery_status;
} pj5682s_cold_state;

/* PASSIVE_LEVEL / blocking wait required. READY means sequence completed,
 * not electrical qualification or authorization to unmute. */
pj5682s_status pj5682s_cold_initialize(pj5682s_cold_state *state,
    const pj5682s_bus *bus, const pj5682s_cold_profile *profile, pj5682s_wait wait);

typedef enum {
    Pj5682sJackPresence,
    Pj5682sJackClassify,
    Pj5682sJackReadClassified
} pj5682s_jack_query;

/* Calls are serialized with bus power and removal by the Windows owner.
 * Start configures AIF1 and its digital routes. The approved analog program
 * applies while the DSP gain is muted. Stop restores the changed fields. A failed rollback can be retried after re-identification. */
pj5682s_status pj5682s_control_start(pj5682s_control *state, const pj5682s_bus *bus,
                                    const pj5682s_control_profile *profile);
/* The clocked variant supports a separately qualified 19.2 MHz PLLB route.
 * The delay is not a PLL lock measurement. The caller keeps the DSP muted. */
pj5682s_status pj5682s_control_start_clocked(pj5682s_control *state, const pj5682s_bus *bus,
    const pj5682s_control_profile *profile, pj5682s_wait clock_wait);
pj5682s_status pj5682s_control_stop(pj5682s_control *state, const pj5682s_bus *bus);
pj5682s_status pj5682s_control_jack(const pj5682s_control *state, const pj5682s_bus *bus,
                                    unsigned detection_completed, pj5682s_jack *jack);
/* Restart the board-qualified JD1 classifier and poll a fresh result. */
pj5682s_status pj5682s_control_classify(pj5682s_control *state, const pj5682s_bus *bus,
    int (*delay_ms)(void *context, unsigned ms), pj5682s_jack *jack);

#ifdef __cplusplus
}
#endif
#endif
