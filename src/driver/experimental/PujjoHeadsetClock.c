/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoHeadsetClock.h"
#ifndef _KERNEL_MODE
#include <string.h>
#endif
pj_clk_status pj_headset_clock_plan(unsigned sample_rate_hz, unsigned channels,
                                    unsigned valid_bits, unsigned slot_bits,
                                    pj_clk_input input_kind, unsigned known_input_hz,
                                    pj_headset_clock *out) {
    if (!out) return PJ_CLK_BAD_FORMAT;
#ifdef _KERNEL_MODE
    RtlZeroMemory(out, sizeof(*out));
#else
    memset(out, 0, sizeof(*out));
#endif
    /* Constrained prototype: expand only after platform clock evidence. */
    if (sample_rate_hz!=48000 || channels!=2 || valid_bits!=24 ||
        (slot_bits!=24 && slot_bits!=32) || slot_bits<valid_bits ||
        (input_kind!=PJ_CLK_BCLK && input_kind!=PJ_CLK_MCLK && input_kind!=PJ_CLK_UNKNOWN))
        return PJ_CLK_BAD_FORMAT;
    out->sample_rate_hz=sample_rate_hz;
    out->channels=channels;
    out->valid_bits=valid_bits;
    out->slot_bits=slot_bits;
    out->bclk_hz=sample_rate_hz*channels*slot_bits;
    out->requested_sysclk_hz=sample_rate_hz*512; /* reference algorithm, not register plan */
    out->input_kind=input_kind;
    out->known_input_hz=known_input_hz;
    if (input_kind==PJ_CLK_UNKNOWN || known_input_hz==0)
        return PJ_CLK_NEED_MEASUREMENT;
    if (input_kind==PJ_CLK_BCLK && known_input_hz!=out->bclk_hz)
        return PJ_CLK_BAD_FORMAT;
    /* Optional PLL2 coefficient planner below; actual clock activation remains gated. */
    out->pll_needed=(known_input_hz!=out->requested_sysclk_hz);
    return PJ_CLK_OK;
}

/* Register addresses/bit fields and direct conversion facts cross-checked
 * against public coolstar/rt5682s registers.h and rt5682s.c. This independent
 * planner does not import that driver's initialization/calibration routines.
 * Provenance and source revisions are in RESEARCH_v0.0.16.md. */
pj_clk_status pj_rt5682s_plan_pll2(unsigned measured_mclk_hz,
    unsigned output_hz, pj_rt5682s_pll2_plan *out)
{
    unsigned m;
    static const pj_clock_register_update coefficients[5] = {
        {0x0080,0x0300,0x0000}, /* MCLK input */
        {0x009a,0x03ff,0x0006}, /* N */
        {0x009b,0x1f1f,0x0003}, /* M filled below; K=3 */
        {0x009d,0x3c00,0x0000}, /* bypass off */
        {0x009e,0x0001,0x0001}  /* direct input */
    };
    if (!out) return PJ_CLK_BAD_FORMAT;
#ifdef _KERNEL_MODE
    RtlZeroMemory(out, sizeof(*out));
#else
    memset(out, 0, sizeof(*out));
#endif
    if (!measured_mclk_hz) return PJ_CLK_NEED_MEASUREMENT;
    if (output_hz != 24576000u) return PJ_CLK_BAD_FORMAT;
    switch (measured_mclk_hz) {
    case 19200000u: m=2; break;
    case 24000000u: m=3; break;
    case 48000000u: m=8; break;
    default: return PJ_CLK_BAD_FORMAT;
    }
    out->input_hz=measured_mclk_hz;out->output_hz=output_hz;
    /* Masked updates preserve unrelated register bits. These coefficients
     * require separate reviewed rail/reset/bias/PLL-lock and sysclk steps. */
#ifdef _KERNEL_MODE
    RtlCopyMemory(out->writes, coefficients, sizeof(coefficients));
#else
    memcpy(out->writes, coefficients, sizeof(coefficients));
#endif
    out->writes[2].value |= (PJ_WORD)(m << 8);
    out->count=5;
    return PJ_CLK_OK;
}
