/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoBringupCoordinator.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    char events[256];
    unsigned count;
    char fail_event;
    int ready;
    unsigned delays;
    unsigned enabled;
    unsigned unmuted;
} fake;
static int event(void *v, char code)
{
    fake *f=(fake *)v;
    if (f->count + 1u < sizeof(f->events)) {
        f->events[f->count++] = code;
        f->events[f->count] = '\0';
    }
    return f->fail_event == code ? -1 : 0;
}
static int mute(void *v) { ((fake *)v)->unmuted=0;return event(v,'M'); }
static int unmute(void *v) { if (event(v,'U')) return -1; ((fake *)v)->unmuted=1;return 0; }
static int amp_off(void *v) { ((fake *)v)->enabled=0;return event(v,'O'); }
static int fw(void *v) { return event(v,'F'); }
static int ssp(void *v) { return event(v,'S'); }
static int gain(void *v) { return event(v,'G'); }
static int amp_on(void *v) { if (event(v,'A'))return -1;((fake *)v)->enabled=1;return 0; }
static int delay(void *v, unsigned t) { fake *f=v; assert(t==300);f->delays++;return event(v,'D'); }
static int ready(void *v) { fake *f=v;(void)event(v,'R');return f->ready; }
static pj_bringup_mock_ops ops(fake *f)
{
    pj_bringup_mock_ops o={f,mute,unmute,amp_off,fw,ssp,gain,amp_on,delay,ready};
    return o;
}
static pj_bringup_evidence valid(void)
{
    pj_bringup_evidence e;
    memset(&e, 0, sizeof(e));
    e.hardware.manufacturer="Google";e.hardware.board="Pujjo";
    e.hardware.pci_vendor=0x8086;e.hardware.pci_device=0x54c8;
    e.hardware.codec_acpi="RTL5682";e.hardware.amp_acpi="RTL1019";
    e.speaker.firmware_ipc_tested=1;
    e.speaker.ssp1_routing_tested=1;
    e.speaker.amp_sdb_gpio_resource_verified=1;
    e.speaker.dsp_speaker_mute_verified=1;
    e.speaker.speaker_gain_limiter_verified=1;
    e.speaker.safe_recovery_available=1;
    e.firmware.board_identity_confirmed=1;
    e.firmware.ipc_protocol_measured=1;
    e.firmware.firmware_signature_checked=1;
    e.firmware.firmware_format_verified=1;
    e.firmware.firmware_dsp_compatibility_tested=1;
    e.firmware.firmware_origin_verified=1;
    e.amplifier.board_and_acpi_verified=1;
    e.amplifier.translated_gpio_verified=1;
    e.amplifier.logical_sdb_polarity_verified=1;
    e.amplifier.speaker_ssp_and_firmware_verified=1;
    e.amplifier.dsp_muting_verified=1;
    e.amplifier.calibration_procedure_verified=1;
    e.amplifier.safe_gain_limit_verified=1;
    e.amplifier.recovery_verified=1;
    e.compatible_ipc4_image_and_windows_loader_verified=1;
    e.windows_ssp1_graph_and_clock_verified=1;
    e.physical_sdb_polarity_and_speaker_limits_measured=1;
    e.windows_pnp_power_and_teardown_verified=1;
    return e;
}
int main(void)
{
    fake f;pj_bringup b;pj_bringup_mock_ops o;pj_bringup_evidence e=valid();
    memset(&f,0,sizeof(f));o=ops(&f);pj_bringup_reset(&b);
    assert(pj_bringup_evidence_complete(&e));
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_WRONG_STATE);
    e.hardware.board="Other";assert(!pj_bringup_evidence_complete(&e));
    e=valid();e.speaker.firmware_ipc_tested=-1;
    assert(!pj_bringup_evidence_complete(&e));
    e=valid();e.amplifier.safe_gain_limit_verified=9;
    assert(!pj_bringup_evidence_complete(&e));
    e=valid();e.windows_pnp_power_and_teardown_verified=0;
    assert(!pj_bringup_evidence_complete(&e));
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_NOT_VERIFIED);
    assert(f.count==0);e=valid();
    e.firmware.firmware_signature_checked=0;
    assert(!pj_bringup_evidence_complete(&e));e=valid();
    e.compatible_ipc4_image_and_windows_loader_verified=2;
    assert(!pj_bringup_evidence_complete(&e));e=valid();
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_LOCKED);
    assert(b.state==PJ_BRINGUP_RESET && f.count==0);
#else
    {
        const char *failures="MOFSG";
        const char *c;
        for (c=failures;*c;c++) {
            pj_bringup_reset(&b);memset(&f,0,sizeof(f)); f.fail_event=*c;
            assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_IO_ERROR);
            assert(b.state==PJ_BRINGUP_FAULT);
            assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_FAULT_LATCHED);
            assert(f.enabled==0 && f.unmuted==0);
        }
        for (c="AD";*c;c++) {
            pj_bringup_reset(&b);memset(&f,0,sizeof(f));
            assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
            f.fail_event=*c;
            assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_IO_ERROR);
            assert(b.state==PJ_BRINGUP_FAULT && !f.enabled && !f.unmuted);
        }
    }
    pj_bringup_reset(&b);memset(&f,0,sizeof(f));o=ops(&f);
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(strcmp(f.events,"MOFSG")==0);
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_WRONG_STATE);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(strcmp(f.events,"MOFSGMAD")==0);
    assert(f.enabled==1 && f.delays==1 && f.unmuted==0);
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_NOT_VERIFIED);
    assert(f.unmuted==0);
    f.ready=1;
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_OK);
    assert(f.enabled && f.unmuted);
    assert(pj_bringup_stop(&b,&o)==PJ_BRINGUP_OK);
    assert(!f.enabled && !f.unmuted);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(f.delays==1); /* no repeated calibration until suspend */
    assert(pj_bringup_suspend(&b,&o)==PJ_BRINGUP_OK);
    assert(!f.enabled && !f.unmuted && !b.ready_epoch);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_WRONG_STATE);
    assert(pj_bringup_resume_to_reset(&b)==PJ_BRINGUP_OK);
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(f.delays==2);
    /* Simulate a failed amplifier power-up; fault is latched. */
    pj_bringup_reset(&b);memset(&f,0,sizeof(f));f.fail_event='A';
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_IO_ERROR);
    assert(b.state==PJ_BRINGUP_FAULT && !f.enabled);
    {
        unsigned before=f.count;
        assert(pj_bringup_stop(&b,&o)==PJ_BRINGUP_FAULT_LATCHED);
        assert(f.count==before+2 && b.state==PJ_BRINGUP_FAULT);
        assert(f.events[f.count-2]=='M' && f.events[f.count-1]=='O');
    }
    /* Ready callback reports I/O failure: latch and mute, do not retry. */
    pj_bringup_reset(&b);memset(&f,0,sizeof(f));f.ready=-1;
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_IO_ERROR);
    assert(b.state==PJ_BRINGUP_FAULT && !f.enabled && !f.unmuted);
    /* DSP unmute failure triggers immediate best-effort shutdown. */
    pj_bringup_reset(&b);memset(&f,0,sizeof(f));f.ready=1;
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    f.fail_event='U';
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_IO_ERROR);
    assert(b.state==PJ_BRINGUP_FAULT && !f.enabled && !f.unmuted);
    /* Fault during shutdown must never be mistaken for a safe stop. */
    pj_bringup_reset(&b);memset(&f,0,sizeof(f));
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    f.fail_event='O';
    assert(pj_bringup_stop(&b,&o)==PJ_BRINGUP_IO_ERROR);
    assert(b.state==PJ_BRINGUP_FAULT);
#endif
    puts("PASS: Pujjo portable bring-up contract (no hardware accessed)");
    return 0;
}
