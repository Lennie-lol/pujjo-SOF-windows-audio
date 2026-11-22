/* SPDX-License-Identifier: BSD-3-Clause */
/* Contract integration: simulated IPC4 FW_READY gates MOCK SSP1 speaker path.
 * All physical-evidence flags below are FICTIONS for host testing only. */
#include "../src/driver/experimental/PujjoIpc4BootModel.h"
#include "../src/driver/experimental/PujjoBringupCoordinator.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    pj_boot_model boot;
    int amp_on;
    int unmuted;
    int mock_stream_ready;
    unsigned delays;
    char trace[128];
    unsigned used;
} mock_world;
static void record(mock_world *m, char ch)
{
    assert(m->used + 1u < sizeof m->trace);
    m->trace[m->used++] = ch;
    m->trace[m->used] = '\0';
}
static int mute(void *v) { mock_world *m = v;record(m,'M');m->unmuted=0;return 0; }
static int unmute(void *v) { mock_world *m = v;record(m,'U');m->unmuted=1;return 0; }
static int amp_off(void *v) { mock_world *m = v;record(m,'O');m->amp_on=0;return 0; }
static int fw_ready(void *v) { mock_world *m = v;record(m,'F');return pj_boot_is_ready(&m->boot) ? 0 : -1; }
static int ssp(void *v) { record(v,'S');return 0; }
static int gain(void *v) { record(v,'G');return 0; }
static int amp_on(void *v) { mock_world *m = v;record(m,'A');m->amp_on=1;return 0; }
static int delay(void *v, unsigned ms) { mock_world *m = v;assert(ms==300u);record(m,'D');m->delays++;return 0; }
static int ready(void *v) { mock_world *m = v;record(m,'R');return m->mock_stream_ready; }
static pj_bringup_mock_ops ops(mock_world *m)
{
    pj_bringup_mock_ops o = {m,mute,unmute,amp_off,fw_ready,ssp,gain,amp_on,delay,ready};
    return o;
}
static pj_boot_evidence boot_evidence(void)
{
    pj_boot_evidence e = {0};
    e.pci_vendor=0x8086u;e.pci_device=0x54c8u;
    e.ipc4_protocol_verified=1u;e.signed_image_verified=1u;
    e.image_layout_verified=1u;e.firmware_dsp_compatible=1u;
    e.dma_resource_contract_verified=1u;
    return e;
}
static pj_bringup_evidence evidence(void)
{
    pj_bringup_evidence e;
    memset(&e,0,sizeof e);
    e.hardware.manufacturer="Google";e.hardware.board="Pujjo";
    e.hardware.pci_vendor=0x8086u;e.hardware.pci_device=0x54c8u;
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
#ifdef PJ_IPC4_HOST_TEST
static const uint8_t valid_caps[] = {
    0,0,0,0, 8,0,0,0, 2,0,12,0,0,0,1,0,
    6,0,0,0, 4,0,0,0, 0,16,0,0,
    7,0,0,0, 4,0,0,0, 0,16,0,0,
    9,0,0,0, 4,0,0,0, 10,0,0,0
};
#endif

int main(void)
{
    mock_world m;
    pj_bringup b;
    pj_bringup_mock_ops o;
    pj_bringup_evidence e = evidence();
    pj_boot_evidence be = boot_evidence();
    memset(&m,0,sizeof m);
    pj_boot_init(&m.boot);
    pj_bringup_reset(&b);
    o=ops(&m);
    assert(pj_bringup_evidence_complete(&e));
#ifndef PJ_BRINGUP_TEST_CALLBACKS
    assert(pj_bringup_prepare(&b,&e,&o) == PJ_BRINGUP_LOCKED);
    assert(!m.amp_on && !m.unmuted);
    assert(pj_boot_stage(&m.boot,&be) == PJ_BOOT_LOCKED);
#else
    uint32_t epoch=0u;
    assert(pj_boot_stage(&m.boot,&be)==PJ_BOOT_OK);
    assert(pj_boot_request(&m.boot,100u,&epoch)==PJ_BOOT_OK);
    /* Boot requested is NOT ready. Must fail and attempt mute+off. */
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_IO_ERROR);
    assert(strcmp(m.trace,"MOFMO")==0);
    assert(!m.amp_on && !m.unmuted && b.state==PJ_BRINGUP_FAULT);
    /* Simulated firmware notification cannot clear the latched fault. */
    assert(pj_boot_notify(&m.boot,epoch,PJ_BOOT_EVENT_FW_READY)==PJ_BOOT_OK);
    assert(!pj_boot_is_ready(&m.boot)); /* FW_READY alone cannot enable amp. */
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_FAULT_LATCHED);
    pj_bringup_reset(&b);
    m.used=0u;m.trace[0]='\0';
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_IO_ERROR);
    assert(!m.amp_on && !m.unmuted);
    assert(pj_boot_confirm_caps(&m.boot,epoch,valid_caps,sizeof(valid_caps))==PJ_BOOT_OK);
    pj_bringup_reset(&b);
    m.used=0u;m.trace[0]='\0';
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(strcmp(m.trace,"MOFSG")==0);
    assert(!m.amp_on && !m.unmuted);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(m.amp_on && !m.unmuted && m.delays==1u);
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_NOT_VERIFIED);
    assert(!m.unmuted);
    m.mock_stream_ready=1;
    assert(pj_bringup_start_stream(&b,&o)==PJ_BRINGUP_OK);
    assert(m.amp_on && m.unmuted && b.state==PJ_BRINGUP_STREAMING);
    assert(pj_bringup_suspend(&b,&o)==PJ_BRINGUP_OK);
    assert(!m.amp_on && !m.unmuted);
    assert(pj_boot_reset_epoch(&m.boot)==PJ_BOOT_OK);
    assert(!pj_boot_is_ready(&m.boot));
    assert(pj_bringup_resume_to_reset(&b)==PJ_BRINGUP_OK);
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_IO_ERROR);
    assert(!m.amp_on && !m.unmuted && b.state==PJ_BRINGUP_FAULT);
    /* A D3 resume requires a new startup epoch; old FW_READY is stale. */
    assert(pj_boot_stage(&m.boot,&be)==PJ_BOOT_OK);
    {
        uint32_t next=0u;
        assert(pj_boot_request(&m.boot,100u,&next)==PJ_BOOT_OK);
        assert(pj_boot_notify(&m.boot,epoch,PJ_BOOT_EVENT_FW_READY)==PJ_BOOT_STALE_EVENT);
        assert(pj_boot_notify(&m.boot,next,PJ_BOOT_EVENT_FW_READY)==PJ_BOOT_OK);
        assert(!pj_boot_is_ready(&m.boot));
        assert(pj_boot_confirm_caps(&m.boot,epoch,valid_caps,sizeof(valid_caps))==PJ_BOOT_STALE_EVENT);
        assert(pj_boot_confirm_caps(&m.boot,next,valid_caps,sizeof(valid_caps))==PJ_BOOT_OK);
    }
    pj_bringup_reset(&b);
    assert(pj_bringup_prepare(&b,&e,&o)==PJ_BRINGUP_OK);
    assert(pj_bringup_amp_on_muted(&b,&o)==PJ_BRINGUP_OK);
    assert(m.delays==2u); /* mock calibration repeats after suspend */
#endif
    puts("PASS: mocked FW_READY -> mute -> SSP1 -> amp sequence; no hardware used");
    return 0;
}
