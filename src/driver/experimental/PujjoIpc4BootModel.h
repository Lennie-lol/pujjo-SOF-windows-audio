/* SPDX-License-Identifier: BSD-3-Clause */
/* HOST-ONLY IPC4 boot lifecycle contract. NOT a real firmware loader.
 * No MMIO/DMA/IPC operation, cryptographic verification, or device access.
 * Evidence fields are test inputs, never measurements. Never link into WDK. */
#ifndef PUJJO_IPC4_BOOT_MODEL_H
#define PUJJO_IPC4_BOOT_MODEL_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PJ_BOOT_OK = 0, PJ_BOOT_BAD_ARG = -1, PJ_BOOT_UNVERIFIED = -2,
    PJ_BOOT_LOCKED = -3, PJ_BOOT_WRONG_STATE = -4,
    PJ_BOOT_STALE_EVENT = -5, PJ_BOOT_TIMEOUT = -6,
    PJ_BOOT_FAULT_LATCHED = -7
} pj_boot_result;
typedef enum {
    PJ_BOOT_RESET = 0, PJ_BOOT_STAGED, PJ_BOOT_WAIT_READY,
    PJ_BOOT_WAIT_CAPS, PJ_BOOT_READY, PJ_BOOT_FAULT
} pj_boot_state;
/* A caller MUST acquire real independent evidence on the physical DUT.
 * Unit tests fill these fields artificially; they do not prove readiness. */
typedef struct {
    uint16_t pci_vendor;
    uint16_t pci_device;
    unsigned ipc4_protocol_verified;
    unsigned signed_image_verified;
    unsigned image_layout_verified;
    unsigned firmware_dsp_compatible;
    unsigned dma_resource_contract_verified;
} pj_boot_evidence;
typedef struct {
    pj_boot_state state;
    uint32_t epoch;
    uint32_t remaining_ms;
} pj_boot_model;

typedef enum { PJ_BOOT_EVENT_UNKNOWN = 0, PJ_BOOT_EVENT_FW_READY = 1 } pj_boot_event;

void pj_boot_init(pj_boot_model *b);
int pj_boot_evidence_complete(const pj_boot_evidence *e);
/* Only host test builds may advance the simulated state. */
pj_boot_result pj_boot_stage(pj_boot_model *b, const pj_boot_evidence *e);
pj_boot_result pj_boot_request(pj_boot_model *b, uint32_t timeout_ms, uint32_t *epoch);
pj_boot_result pj_boot_notify(pj_boot_model *b, uint32_t epoch, pj_boot_event event);
/* FW_READY alone is never READY: the matching boot epoch must also supply a
 * validated FW_CONFIG reply with both 4096-byte mailboxes and >=2 pipelines.
 * Host-test only. No real DSP reply authentication or I/O. */
pj_boot_result pj_boot_confirm_caps(pj_boot_model *b, uint32_t epoch,
                                    const uint8_t *fw_config, size_t bytes);
pj_boot_result pj_boot_advance_time(pj_boot_model *b, uint32_t elapsed_ms);
pj_boot_result pj_boot_abort(pj_boot_model *b);
/* Invalidate outstanding readiness events across mock D3/removal/restart. */
pj_boot_result pj_boot_reset_epoch(pj_boot_model *b);
int pj_boot_is_ready(const pj_boot_model *b);
#ifdef __cplusplus
}
#endif
#endif
