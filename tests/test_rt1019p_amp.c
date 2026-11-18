/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoRt1019pAmp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct fake {
    char events[128];
    unsigned count;
    unsigned sleeps;
    unsigned last_sleep;
    int fail_mute;
    int fail_gpio;
    int fail_sleep;
} fake;
static void event(fake *f, char c) {
    assert(f->count + 1 < sizeof(f->events));
    f->events[f->count++] = c;
    f->events[f->count] = 0;
}
static int mute(void *ctx, int muted) {
    fake *f=(fake *)ctx;
    event(f, muted ? 'M' : 'U');
    return f->fail_mute ? -1 : 0;
}
static int gpio(void *ctx, int enabled) {
    fake *f=(fake *)ctx;
    event(f, enabled ? 'E' : 'D');
    return f->fail_gpio ? -1 : 0;
}
static int sleep_ms(void *ctx, unsigned ms) {
    fake *f=(fake *)ctx;
    event(f, 'W'); f->sleeps++; f->last_sleep=ms;
    return f->fail_sleep ? -1 : 0;
}
static pj1019_evidence all_verified(void) {
    pj1019_evidence e={1,1,1,1,1,1,1,1};
    return e;
}
static pj1019_mock_io callbacks(fake *f) {
    pj1019_mock_io io={f,mute,gpio,sleep_ms};
    return io;
}
#ifndef PJ1019_TEST_ONLY_CALLBACKS
static void locked_tests(void) {
    pj1019_amp a;
    pj1019_evidence e=all_verified();
    fake f={0};
    pj1019_mock_io io=callbacks(&f);
    pj1019_reset(&a);
    assert(a.state==PJ1019_UNINITIALIZED && a.calibration_required==1);
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_LOCKED);
    assert(f.count==0 && a.state==PJ1019_UNINITIALIZED);
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_BAD_STATE);
    assert(pj1019_audio_start(&a,&io,1)==PJ1019_BAD_STATE);
    assert(pj1019_suspend(&a,&io)==PJ1019_BAD_STATE);
    e.logical_sdb_polarity_verified=0;
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_UNVERIFIED);
    assert(pj1019_prepare(NULL,&e,&io)==PJ1019_INVALID);
    assert(f.count==0);
}
#else
static void happy_path(void) {
    pj1019_amp a;
    pj1019_evidence e=all_verified();
    fake f={0};
    pj1019_mock_io io=callbacks(&f);
    pj1019_reset(&a);
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_OK);
    assert(!strcmp(f.events,"MD"));
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_OK);
    assert(a.state==PJ1019_ON_MUTED && f.sleeps==1 && f.last_sleep==300);
    assert(!strcmp(f.events,"MDMEW"));
    assert(pj1019_audio_start(&a,&io,0)==PJ1019_UNVERIFIED);
    assert(a.state==PJ1019_ON_MUTED && !strcmp(f.events,"MDMEW"));
    assert(pj1019_audio_start(&a,&io,1)==PJ1019_OK);
    assert(!strcmp(f.events,"MDMEWU"));
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_BAD_STATE);
    assert(pj1019_stop(&a,&io)==PJ1019_OK);
    assert(a.state==PJ1019_OFF_MUTED && !strcmp(f.events,"MDMEWUMD"));
    /* Normal stop retains calibration. */
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_OK);
    assert(f.sleeps==1 && a.state==PJ1019_ON_MUTED);
    /* Suspend requires mute->SDB disable and loses calibration. */
    assert(pj1019_suspend(&a,&io)==PJ1019_OK);
    assert(a.state==PJ1019_SUSPENDED && a.calibration_required);
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_BAD_STATE);
    assert(pj1019_resume(&a)==PJ1019_OK);
    assert(a.state==PJ1019_OFF_MUTED);
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_OK);
    assert(f.sleeps==2 && f.last_sleep==300);
    assert(pj1019_stop(&a,&io)==PJ1019_OK);
}
static void fail_closed(void) {
    pj1019_amp a;
    pj1019_evidence e=all_verified();
    fake f={0};
    pj1019_mock_io io=callbacks(&f);
    pj1019_reset(&a);
    e.calibration_procedure_verified=0;
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_UNVERIFIED && !f.count);
    e.calibration_procedure_verified=1;
    e.safe_gain_limit_verified=0;
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_UNVERIFIED && !f.count);
    e.safe_gain_limit_verified=1;
    f.fail_gpio=1;
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_IO_FAILURE);
    assert(a.state==PJ1019_FAULT);
    assert(!strcmp(f.events,"MDMD"));
    /* Cannot enable when faulted. */
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_BAD_STATE);
    pj1019_reset(&a); memset(&f,0,sizeof(f));
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_OK);
    f.fail_sleep=1;
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_IO_FAILURE);
    assert(a.state==PJ1019_FAULT && a.calibration_required);
    assert(!strcmp(f.events,"MDMEWMD"));
    pj1019_reset(&a); memset(&f,0,sizeof(f));
    assert(pj1019_prepare(&a,&e,&io)==PJ1019_OK);
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_OK);
    f.fail_mute=1;
    assert(pj1019_stop(&a,&io)==PJ1019_IO_FAILURE);
    assert(a.state==PJ1019_FAULT);
    /* Even when mute is broken, GPIO disable MUST be attempted. */
    assert(f.events[f.count-1]=='D');
    assert(pj1019_resume(&a)==PJ1019_BAD_STATE);
    f.fail_mute=0;
    /* Retrying shutdown must still mute/disable, but never clear FAULT. */
    assert(pj1019_stop(&a,&io)==PJ1019_IO_FAILURE);
    assert(a.state==PJ1019_FAULT && f.events[f.count-1]=='D');
    assert(pj1019_suspend(&a,&io)==PJ1019_IO_FAILURE);
    assert(a.state==PJ1019_FAULT);
    assert(pj1019_resume(&a)==PJ1019_BAD_STATE);
    assert(pj1019_power_on_muted(&a,&io)==PJ1019_BAD_STATE);
}
#endif
int main(void) {
#ifndef PJ1019_TEST_ONLY_CALLBACKS
    locked_tests();
    puts("PASS: RT1019P physical GPIO disabled by default");
#else
    happy_path(); fail_closed();
    puts("PASS: RT1019P mock-only sequencing, errors, suspend/resume and mute order");
#endif
    return 0;
}
