/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_SOF_FIRMWARE_POLICY_H
#define PUJJO_SOF_FIRMWARE_POLICY_H
#include <stddef.h>
#include <stdint.h>

/* Candidate firmware paths are Linux sof-bin *source paths*, not paths installed
 * by Windows or proof of a verified IPC4 image. No hardware access here. */
typedef enum {
    PJ_SOF_FW_OK = 0,
    PJ_SOF_FW_INVALID_ARGUMENT,
    PJ_SOF_FW_UNSUPPORTED_BOARD,
    PJ_SOF_FW_UNVERIFIED_PROTOCOL,
    PJ_SOF_FW_NO_SPACE
} PJ_SOF_FW_RESULT;

typedef enum { PJ_SOF_IPC_UNKNOWN = 0, PJ_SOF_IPC3 = 3, PJ_SOF_IPC4 = 4 } PJ_SOF_IPC;

/* Only accepts the Pujjo N100 PCI device. Explicit IPC selection required.
 * Output relative to Intel SOF firmware repository, not an NT filesystem path. */
PJ_SOF_FW_RESULT pj_sof_candidate_firmware(uint16_t vendor, uint16_t device,
                                           PJ_SOF_IPC ipc,
                                           char *output, size_t output_cap);

/* No caller can assert physical compatibility simply by finding a filename.
 * This helper lists independent review steps; it does not examine hardware. */
typedef struct {
    unsigned board_identity_confirmed : 1;
    unsigned ipc_protocol_measured : 1;
    unsigned firmware_signature_checked : 1;
    unsigned firmware_format_verified : 1;
    unsigned firmware_dsp_compatibility_tested : 1;
    unsigned firmware_origin_verified : 1;
} PJ_SOF_FW_EVIDENCE;
int pj_sof_evidence_complete(const PJ_SOF_FW_EVIDENCE *evidence);
#endif
