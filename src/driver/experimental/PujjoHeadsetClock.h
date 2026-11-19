/* SPDX-License-Identifier: BSD-3-Clause */
/* Read-only clock/format negotiation for the *headset* RT5682S on SSP0.
 * It performs NO register writes or I2C transfers. */
#ifndef PUJJO_HEADSET_CLOCK_H
#define PUJJO_HEADSET_CLOCK_H
#include "../PujjoFixedTypes.h"
typedef enum pj_clk_input { PJ_CLK_UNKNOWN=0, PJ_CLK_MCLK=1, PJ_CLK_BCLK=2 } pj_clk_input;
typedef enum pj_clk_status { PJ_CLK_OK=0, PJ_CLK_BAD_FORMAT=-1, PJ_CLK_NEED_MEASUREMENT=-2 } pj_clk_status;
typedef struct pj_headset_clock {
    unsigned sample_rate_hz;
    unsigned channels;
    unsigned valid_bits;
    unsigned slot_bits;
    unsigned bclk_hz;
    unsigned requested_sysclk_hz;
    unsigned known_input_hz;
    pj_clk_input input_kind;
    int pll_needed;
} pj_headset_clock;
pj_clk_status pj_headset_clock_plan(unsigned sample_rate_hz, unsigned channels,
                                    unsigned valid_bits, unsigned slot_bits,
                                    pj_clk_input input_kind, unsigned known_input_hz,
                                    pj_headset_clock *out);

/* Pure coefficient planning, never powers PLL or switches sysclk.
 * Measured MCLK is mandatory; only three documented direct PLLB conversions
 * to 24.576 MHz are represented. This is NOT a full codec startup recipe. */
typedef struct pj_clock_register_update {
    PJ_WORD reg, mask, value;
} pj_clock_register_update;
typedef struct pj_rt5682s_pll2_plan {
    unsigned input_hz, output_hz, count;
    pj_clock_register_update writes[5];
} pj_rt5682s_pll2_plan;
pj_clk_status pj_rt5682s_plan_pll2(unsigned measured_mclk_hz,
    unsigned output_hz, pj_rt5682s_pll2_plan *out);
/* RT5682S AIF1 stereo I2S with the codec receiving both clocks. The caller
 * supplies measured frame geometry and must independently establish that
 * the link uses I2S. Updates configure framing only and never touch power. */
typedef struct pj_rt5682s_i2s_plan {
    unsigned count;
    pj_clock_register_update writes[3];
} pj_rt5682s_i2s_plan;
pj_clk_status pj_rt5682s_plan_i2s_receiver(unsigned slot_bits, unsigned measured_bclk_hz,
    unsigned measured_lrclk_hz, unsigned inverted_bclk, pj_rt5682s_i2s_plan *out);
#endif
