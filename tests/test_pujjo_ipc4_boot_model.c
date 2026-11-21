/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-only in-memory state model. Never generates an IPC packet. */
#include "../src/driver/experimental/PujjoIpc4BootModel.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static pj_boot_evidence good(void)
{
    pj_boot_evidence e = {0};
    e.pci_vendor = 0x8086u; e.pci_device = 0x54c8u;
    e.ipc4_protocol_verified = 1u; e.signed_image_verified = 1u;
    e.image_layout_verified = 1u; e.firmware_dsp_compatible = 1u;
    e.dma_resource_contract_verified = 1u;
    return e;
}
static const uint8_t valid_caps[] = {
    0,0,0,0, 8,0,0,0, 2,0,12,0,0,0,1,0, /* version */
    6,0,0,0, 4,0,0,0, 0,16,0,0,         /* DL=4096 */
    7,0,0,0, 4,0,0,0, 0,16,0,0,         /* UL=4096 */
    9,0,0,0, 4,0,0,0, 10,0,0,0          /* 10 pipelines */
};

int main(void)
{
    pj_boot_model b;
    pj_boot_evidence e = good();
    pj_boot_init(&b);
    assert(b.state == PJ_BOOT_RESET && !pj_boot_is_ready(&b));
    assert(!pj_boot_evidence_complete(NULL));
    assert(pj_boot_stage(NULL, &e) == PJ_BOOT_BAD_ARG);
    assert(pj_boot_stage(&b, NULL) == PJ_BOOT_BAD_ARG);
    e.pci_device = 0u;
    assert(!pj_boot_evidence_complete(&e));
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_UNVERIFIED && b.state == PJ_BOOT_RESET);
    e = good();e.ipc4_protocol_verified = 2u;
    assert(!pj_boot_evidence_complete(&e));
    e = good();e.dma_resource_contract_verified = 0u;
    assert(!pj_boot_evidence_complete(&e));
    e = good();
#ifndef PJ_IPC4_HOST_TEST
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_LOCKED);
    assert(b.state == PJ_BOOT_RESET && !pj_boot_is_ready(&b));
#else
    uint32_t generation = 42u, previous;
    assert(pj_boot_notify(&b, 1u, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_WRONG_STATE);
    assert(pj_boot_request(&b, 10u, &generation) == PJ_BOOT_WRONG_STATE);
    assert(generation == 0u);
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_WRONG_STATE);
    assert(pj_boot_request(&b, 0u, &generation) == PJ_BOOT_BAD_ARG);
    assert(pj_boot_request(&b, 60001u, &generation) == PJ_BOOT_BAD_ARG);
    assert(pj_boot_request(&b, 100u, &generation) == PJ_BOOT_OK);
    assert(generation != 0u && !pj_boot_is_ready(&b));
    assert(pj_boot_notify(&b, generation - 1u, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_STALE_EVENT);
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_UNKNOWN) == PJ_BOOT_STALE_EVENT);
    assert(b.state == PJ_BOOT_WAIT_READY);
    assert(pj_boot_advance_time(&b, 99u) == PJ_BOOT_OK);
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_OK);
    assert(b.state == PJ_BOOT_WAIT_CAPS && !pj_boot_is_ready(&b));
    assert(pj_boot_confirm_caps(&b, generation - 1u, valid_caps, sizeof(valid_caps)) == PJ_BOOT_STALE_EVENT);
    assert(pj_boot_advance_time(&b, 1u) == PJ_BOOT_TIMEOUT);
    assert(!pj_boot_is_ready(&b) && b.state == PJ_BOOT_FAULT);
    assert(pj_boot_reset_epoch(&b) == PJ_BOOT_OK);
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
    assert(pj_boot_request(&b, 100u, &generation) == PJ_BOOT_OK);
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_OK);
    assert(pj_boot_confirm_caps(&b, generation, valid_caps, sizeof(valid_caps)) == PJ_BOOT_OK);
    assert(pj_boot_is_ready(&b));
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_WRONG_STATE);
    /* D3/removal reset MUST invalidate the previous firmware-ready event. */
    previous = generation;
    assert(pj_boot_reset_epoch(&b) == PJ_BOOT_OK);
    assert(!pj_boot_is_ready(&b));
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
    assert(pj_boot_request(&b, 20u, &generation) == PJ_BOOT_OK);
    assert(generation != previous);
    assert(pj_boot_notify(&b, previous, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_STALE_EVENT);
    assert(pj_boot_advance_time(&b, 20u) == PJ_BOOT_TIMEOUT);
    assert(b.state == PJ_BOOT_FAULT && !pj_boot_is_ready(&b));
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_FAULT_LATCHED);
    assert(pj_boot_advance_time(&b, 1u) == PJ_BOOT_FAULT_LATCHED);
    assert(pj_boot_reset_epoch(&b) == PJ_BOOT_OK);
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
    assert(pj_boot_request(&b, 100u, &generation) == PJ_BOOT_OK);
    assert(pj_boot_abort(&b) == PJ_BOOT_OK);
    assert(!pj_boot_is_ready(&b) && b.state == PJ_BOOT_FAULT);
    /* Malformed, insufficient and duplicate FW_CONFIG cannot authorize audio. */
    for (unsigned fault = 0; fault < 4; ++fault) {
        uint8_t bad[sizeof(valid_caps)];
        for (unsigned i=0;i<sizeof(bad);++i) bad[i] = valid_caps[i];
        assert(pj_boot_reset_epoch(&b) == PJ_BOOT_OK);
        assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
        assert(pj_boot_request(&b, 100u, &generation) == PJ_BOOT_OK);
        assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_OK);
        if (fault == 0u) bad[25] = 8u; /* downlink <4096 */
        if (fault == 1u) bad[37] = 8u; /* uplink <4096 */
        if (fault == 2u) bad[48] = 1u; /* <2 pipelines */
        if (fault == 3u) bad[40] = 7u; /* duplicate UL, missing pipeline */
        assert(pj_boot_confirm_caps(&b, generation, bad, sizeof(bad)) == PJ_BOOT_UNVERIFIED);
        assert(!pj_boot_is_ready(&b) && b.state == PJ_BOOT_FAULT);
        assert(pj_boot_confirm_caps(&b, generation, valid_caps, sizeof(valid_caps)) == PJ_BOOT_FAULT_LATCHED);
    }
    /* A complete FW_READY without a FW_CONFIG reply also times out. */
    assert(pj_boot_reset_epoch(&b) == PJ_BOOT_OK);
    assert(pj_boot_stage(&b, &e) == PJ_BOOT_OK);
    assert(pj_boot_request(&b, 5u, &generation) == PJ_BOOT_OK);
    assert(pj_boot_notify(&b, generation, PJ_BOOT_EVENT_FW_READY) == PJ_BOOT_OK);
    assert(pj_boot_advance_time(&b, 5u) == PJ_BOOT_TIMEOUT);
    assert(!pj_boot_is_ready(&b));
    /* Exhausted generations fail closed, rather than reusing stale tokens. */
    b.epoch = UINT32_MAX;
    assert(pj_boot_reset_epoch(&b) == PJ_BOOT_FAULT_LATCHED);
    assert(b.state == PJ_BOOT_FAULT);
#endif
    puts("PASS: host-only IPC4 boot contract, no firmware or hardware access");
    return 0;
}
