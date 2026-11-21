/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoIpc4BootModel.h"
#include "../PujjoIpc4Protocol.h"
#include <limits.h>
#if defined(PJ_IPC4_HOST_TEST) && (defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDF_H_))
#error "PJ_IPC4_HOST_TEST must never be enabled in a kernel build"
#endif

void pj_boot_init(pj_boot_model *b)
{
    if (!b) return;
    b->state = PJ_BOOT_RESET;
    b->epoch = 1u;
    b->remaining_ms = 0u;
}
int pj_boot_evidence_complete(const pj_boot_evidence *e)
{
    return e && e->pci_vendor == UINT16_C(0x8086) &&
        e->pci_device == UINT16_C(0x54c8) &&
        e->ipc4_protocol_verified == 1u &&
        e->signed_image_verified == 1u &&
        e->image_layout_verified == 1u &&
        e->firmware_dsp_compatible == 1u &&
        e->dma_resource_contract_verified == 1u;
}
pj_boot_result pj_boot_stage(pj_boot_model *b, const pj_boot_evidence *e)
{
    if (!b || !e) return PJ_BOOT_BAD_ARG;
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
    if (b->state != PJ_BOOT_RESET) return PJ_BOOT_WRONG_STATE;
    if (!pj_boot_evidence_complete(e)) return PJ_BOOT_UNVERIFIED;
#ifndef PJ_IPC4_HOST_TEST
    return PJ_BOOT_LOCKED;
#else
    b->state = PJ_BOOT_STAGED;
    return PJ_BOOT_OK;
#endif
}
pj_boot_result pj_boot_request(pj_boot_model *b, uint32_t timeout_ms, uint32_t *epoch)
{
    if (!b || !epoch || !timeout_ms || timeout_ms > 60000u) return PJ_BOOT_BAD_ARG;
    *epoch = 0u; /* A failure never returns a usable generation. */
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
    if (b->state != PJ_BOOT_STAGED) return PJ_BOOT_WRONG_STATE;
#ifndef PJ_IPC4_HOST_TEST
    return PJ_BOOT_LOCKED;
#else
    if (b->epoch == UINT32_MAX) {
        b->state = PJ_BOOT_FAULT; /* Never recycle event tokens. */
        return PJ_BOOT_FAULT_LATCHED;
    }
    ++b->epoch;
    b->remaining_ms = timeout_ms;
    b->state = PJ_BOOT_WAIT_READY;
    *epoch = b->epoch;
    return PJ_BOOT_OK;
#endif
}
pj_boot_result pj_boot_notify(pj_boot_model *b, uint32_t epoch, pj_boot_event event)
{
    if (!b) return PJ_BOOT_BAD_ARG;
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
    if (b->state != PJ_BOOT_WAIT_READY) return PJ_BOOT_WRONG_STATE;
    if (epoch == 0u || epoch != b->epoch || event != PJ_BOOT_EVENT_FW_READY)
        return PJ_BOOT_STALE_EVENT;
#ifndef PJ_IPC4_HOST_TEST
    return PJ_BOOT_LOCKED;
#else
    if (b->remaining_ms == 0u) {
        b->state = PJ_BOOT_FAULT;
        return PJ_BOOT_TIMEOUT;
    }
    b->state = PJ_BOOT_WAIT_CAPS; /* Real firmware also requires FW_CONFIG. */
    return PJ_BOOT_OK;
#endif
}
/* Host-only model of the real boot-admission FW_CONFIG parser.
 * Token is a host-side epoch and is not sent over the IPC4 wire. */
pj_boot_result pj_boot_confirm_caps(pj_boot_model *b, uint32_t epoch,
                                    const uint8_t *fw_config, size_t bytes)
{
    if (!b) return PJ_BOOT_BAD_ARG;
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
    if (b->state != PJ_BOOT_WAIT_CAPS) return PJ_BOOT_WRONG_STATE;
    if (!epoch || epoch != b->epoch) return PJ_BOOT_STALE_EVENT;
#ifndef PJ_IPC4_HOST_TEST
    (void)fw_config;
    (void)bytes;
    return PJ_BOOT_LOCKED;
#else
    pj_ipc4_boot_caps caps;
    if (!b->remaining_ms) {
        b->state = PJ_BOOT_FAULT;
        return PJ_BOOT_TIMEOUT;
    }
    if (pj_ipc4_fw_boot_caps(fw_config, bytes, 4096u, 2u, &caps) != PJ_IPC4_VALID) {
        b->state = PJ_BOOT_FAULT;
        b->remaining_ms = 0u;
        return PJ_BOOT_UNVERIFIED;
    }
    b->remaining_ms = 0u;
    b->state = PJ_BOOT_READY;
    return PJ_BOOT_OK;
#endif
}
pj_boot_result pj_boot_advance_time(pj_boot_model *b, uint32_t elapsed_ms)
{
    if (!b) return PJ_BOOT_BAD_ARG;
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
    if (b->state != PJ_BOOT_WAIT_READY && b->state != PJ_BOOT_WAIT_CAPS)
        return PJ_BOOT_WRONG_STATE;
#ifndef PJ_IPC4_HOST_TEST
    (void)elapsed_ms;
    return PJ_BOOT_LOCKED;
#else
    if (elapsed_ms >= b->remaining_ms) {
        b->remaining_ms = 0u;
        b->state = PJ_BOOT_FAULT;
        return PJ_BOOT_TIMEOUT;
    }
    b->remaining_ms -= elapsed_ms;
    return PJ_BOOT_OK;
#endif
}
pj_boot_result pj_boot_abort(pj_boot_model *b)
{
    if (!b) return PJ_BOOT_BAD_ARG;
    if (b->state == PJ_BOOT_FAULT) return PJ_BOOT_FAULT_LATCHED;
#ifndef PJ_IPC4_HOST_TEST
    return PJ_BOOT_LOCKED;
#else
    b->state = PJ_BOOT_FAULT;
    b->remaining_ms = 0u;
    return PJ_BOOT_OK;
#endif
}
pj_boot_result pj_boot_reset_epoch(pj_boot_model *b)
{
    if (!b) return PJ_BOOT_BAD_ARG;
#ifndef PJ_IPC4_HOST_TEST
    return PJ_BOOT_LOCKED;
#else
    if (b->epoch == UINT32_MAX) {
        b->state = PJ_BOOT_FAULT;
        b->remaining_ms = 0u;
        return PJ_BOOT_FAULT_LATCHED;
    }
    ++b->epoch;
    b->remaining_ms = 0u;
    b->state = PJ_BOOT_RESET;
    return PJ_BOOT_OK;
#endif
}
int pj_boot_is_ready(const pj_boot_model *b)
{
    return b && b->state == PJ_BOOT_READY;
}
