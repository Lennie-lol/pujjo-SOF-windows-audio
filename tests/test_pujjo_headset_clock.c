/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoHeadsetClock.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    pj_headset_clock p;
    assert(pj_headset_clock_plan(48000,2,24,32,PJ_CLK_UNKNOWN,0,&p)==PJ_CLK_NEED_MEASUREMENT);
    assert(p.bclk_hz==3072000 && p.requested_sysclk_hz==24576000);
    assert(pj_headset_clock_plan(48000,2,24,32,PJ_CLK_BCLK,3072000,&p)==PJ_CLK_OK);
    assert(p.pll_needed==1);
    assert(pj_headset_clock_plan(48000,2,24,24,PJ_CLK_BCLK,2304000,&p)==PJ_CLK_OK);
    assert(p.bclk_hz==2304000);
    assert(pj_headset_clock_plan(48000,2,24,25,PJ_CLK_BCLK,2400000,&p)==PJ_CLK_OK);
    assert(p.bclk_hz==2400000 && p.slot_bits==25 && p.pll_needed);
    assert(pj_headset_clock_plan(48000,2,24,25,PJ_CLK_BCLK,2304000,&p)==PJ_CLK_BAD_FORMAT);
    assert(pj_headset_clock_plan(48000,2,24,25,PJ_CLK_MCLK,19200000,&p)==PJ_CLK_OK);
    assert(p.bclk_hz==2400000 && p.requested_sysclk_hz==24576000 && p.pll_needed);
    assert(pj_headset_clock_plan(48000,2,24,32,PJ_CLK_MCLK,24576000,&p)==PJ_CLK_OK);
    assert(p.pll_needed==0);
    assert(pj_headset_clock_plan(48000,2,24,32,PJ_CLK_BCLK,2304000,&p)==PJ_CLK_BAD_FORMAT);
    assert(pj_headset_clock_plan(48000,2,24,20,PJ_CLK_UNKNOWN,0,&p)==PJ_CLK_BAD_FORMAT);
    assert(pj_headset_clock_plan(96000,2,24,32,PJ_CLK_UNKNOWN,0,&p)==PJ_CLK_BAD_FORMAT);
    assert(pj_headset_clock_plan(48000,2,24,32,PJ_CLK_UNKNOWN,0,0)==PJ_CLK_BAD_FORMAT);
    pj_rt5682s_pll2_plan plan;
    const unsigned clocks[]={19200000u,24000000u,48000000u};
    const uint16_t expected_mk[]={0x0203,0x0303,0x0803};
    for(unsigned i=0;i<3;++i) {
        assert(pj_rt5682s_plan_pll2(clocks[i],24576000u,&plan)==PJ_CLK_OK);
        assert(plan.count==5 && plan.writes[2].value==expected_mk[i]);
        for(unsigned j=0;j<plan.count;++j) {
            const pj_clock_register_update *w=&plan.writes[j];
            assert((w->value & (uint16_t)~w->mask)==0);
            uint16_t old=0xa55a;
            uint16_t updated=(uint16_t)((old & (uint16_t)~w->mask)|w->value);
            assert((updated & (uint16_t)~w->mask)==(old & (uint16_t)~w->mask));
        }
    }
    assert(pj_rt5682s_plan_pll2(0,24576000,&plan)==PJ_CLK_NEED_MEASUREMENT && !plan.count);
    assert(pj_rt5682s_plan_pll2(3072000,24576000,&plan)==PJ_CLK_BAD_FORMAT && !plan.count);
    assert(pj_rt5682s_plan_pll2(24000000,22579200,&plan)==PJ_CLK_BAD_FORMAT && !plan.count);
    assert(pj_rt5682s_plan_pll2(24000000,24576000,NULL)==PJ_CLK_BAD_FORMAT);
    pj_rt5682s_i2s_plan i2s;
    /* Do not invent a register encoding for 25 physical clocks. */
    assert(pj_rt5682s_plan_i2s_receiver(25,2400000,48000,0,&i2s)==PJ_CLK_BAD_FORMAT && !i2s.count);
    assert(pj_rt5682s_plan_i2s_receiver(32, 0, 48000, 0,&i2s)==PJ_CLK_NEED_MEASUREMENT);
    assert(i2s.count==0);
    assert(pj_rt5682s_plan_i2s_receiver(32, 2304000, 48000, 0,&i2s)==PJ_CLK_BAD_FORMAT);
    assert(pj_rt5682s_plan_i2s_receiver(32, 3072000, 48000, 2,&i2s)==PJ_CLK_BAD_FORMAT);
    assert(pj_rt5682s_plan_i2s_receiver(32, 3072000, 48000, 1,&i2s)==PJ_CLK_OK);
    assert(i2s.count==3 && i2s.writes[0].value==0);
    assert(i2s.writes[1].value==0x3320 && i2s.writes[2].value==0x8030);
    assert(pj_rt5682s_plan_i2s_receiver(24, 2304000, 48000, 0,&i2s)==PJ_CLK_OK);
    assert(i2s.writes[1].value==0x2220 && i2s.writes[2].value==0x0020);
    for (unsigned i=0; i<i2s.count; ++i)
        assert((i2s.writes[i].value & (uint16_t)~i2s.writes[i].mask)==0);
    puts("PASS: RT5682S headset-only clock plan; no inferred input clock or register writes");
    return 0;
}
