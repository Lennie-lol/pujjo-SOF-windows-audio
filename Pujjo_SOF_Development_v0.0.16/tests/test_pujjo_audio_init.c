/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoAudioInit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct mock { char calls[96]; unsigned delay; int fail_call; int writes; } mock;
static void mark(mock *m, char code) {
    size_t n = strlen(m->calls); assert(n+1 < sizeof(m->calls));
    m->calls[n] = code; m->calls[n+1] = 0;
}
static int write_op(mock *m, char code) {
    mark(m, code); ++m->writes;
    return (m->fail_call && m->writes == m->fail_call) ? -1 : 0;
}
static int amp(void *p,int on) { return write_op((mock*)p, on ? '+' : '-'); }
static int dsp_mute(void *p,int muted) { return write_op((mock*)p, muted ? 'M' : 'U'); }
static int speaker(void *p,unsigned ssp,unsigned hz,unsigned ch,unsigned bits) {
    assert(ssp == 1 && hz == 48000 && ch == 2 && bits == 24);
    return write_op((mock*)p,'L');
}
static void delay(void *p,unsigned ms) { ((mock*)p)->delay += ms; mark((mock*)p,'D'); }
static pj_ops ops(mock *m) { pj_ops o={amp,dsp_mute,speaker,delay,m}; return o; }
static pj_verification all_verified(void) { pj_verification v={1,1,1,1,1,1}; return v; }
int main(void) {
    pj_hardware h={"Google","Pujjo",0x8086,0x54c8,"RTL5682","RTL1019"};
    pj_plan p; pj_audio a; pj_verification v=all_verified();
    mock m={{0},0,0,0}; pj_ops o=ops(&m);
    assert(pj_build_plan(&h,&p)==PJ_OK);
    assert(p.speaker_ssp==1 && p.headset_ssp==0);
    h.board="Pujjo1e"; assert(pj_build_plan(&h,&p)==PJ_OK);
    h.board="Brya"; assert(pj_build_plan(&h,&p)==PJ_E_WRONG_BOARD);
    h.board="Pujjo"; h.amp_acpi="RTL1015";
    assert(pj_build_plan(&h,&p)==PJ_E_MISMATCH);
    h.amp_acpi="RTL1019"; assert(pj_build_plan(&h,&p)==PJ_OK);
    assert(!pj_has_verified_resources(NULL));
    pj_audio_reset(&a);
    v.speaker_gain_limiter_verified=0;
    assert(pj_audio_prepare(&a,&p,&v,&o)==PJ_E_UNVERIFIED);
    assert(m.writes==0); v.speaker_gain_limiter_verified=1;
#ifdef PJ_AUDIO_ENABLE_EXPERIMENTAL_IO
    assert(pj_audio_prepare(&a,&p,&v,&o)==PJ_OK);
    assert(strcmp(m.calls,"-ML")==0);
    assert(a.state==PJ_STATE_PREPARED_MUTED);
    assert(pj_audio_start(&a,&o)==PJ_OK);
    assert(strcmp(m.calls,"-ML+DU")==0);
    assert(m.delay==300 && a.state==PJ_STATE_PLAYING);
    assert(pj_audio_stop(&a,&o)==PJ_OK);
    assert(strcmp(m.calls,"-ML+DUM-")==0);
    assert(pj_audio_start(&a,&o)==PJ_OK);
    assert(m.delay==300);
    pj_audio_reset(&a); memset(&m,0,sizeof(m)); m.fail_call=3;
    assert(pj_audio_prepare(&a,&p,&v,&o)==PJ_E_IO);
    assert(a.state==PJ_STATE_FAULT_MUTED);
    assert(strcmp(m.calls,"-MLM-")==0);
    assert(pj_audio_start(&a,&o)==PJ_E_STATE);
#else
    assert(pj_audio_prepare(&a,&p,&v,&o)==PJ_E_NO_BACKEND);
    assert(pj_audio_start(&a,&o)==PJ_E_STATE);
    assert(m.writes==0 && strlen(m.calls)==0);
#endif
    puts("PASS: speaker-only SSP1 sequence; RT5682S headset operations absent");
    return 0;
}
