/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoSofFirmwarePolicy.h"
#include <string.h>
PJ_SOF_FW_RESULT pj_sof_candidate_firmware(uint16_t vendor, uint16_t device,
                                           PJ_SOF_IPC ipc,
                                           char *output, size_t output_cap)
{
    const char *candidate;
    size_t n;
    if (!output || output_cap == 0u) return PJ_SOF_FW_INVALID_ARGUMENT;
    output[0] = '\0'; /* No partial/ambiguous output after failures. */
    if (vendor != UINT16_C(0x8086) || device != UINT16_C(0x54c8))
        return PJ_SOF_FW_UNSUPPORTED_BOARD;
    /* Do NOT auto-choose an IPC protocol on the basis of a file name or PCI ID. */
    if (ipc == PJ_SOF_IPC3)
        candidate = "sof/intel-signed/sof-adl-n.ri";
    else if (ipc == PJ_SOF_IPC4)
        candidate = "sof-ipc4/adl-n/intel-signed/sof-adl-n.ri";
    else
        return PJ_SOF_FW_UNVERIFIED_PROTOCOL;
    n = strlen(candidate);
    if (n + 1u > output_cap) return PJ_SOF_FW_NO_SPACE;
    memcpy(output, candidate, n + 1u);
    return PJ_SOF_FW_OK;
}
int pj_sof_evidence_complete(const PJ_SOF_FW_EVIDENCE *e)
{
    return e && e->board_identity_confirmed && e->ipc_protocol_measured &&
           e->firmware_signature_checked && e->firmware_format_verified &&
           e->firmware_dsp_compatibility_tested && e->firmware_origin_verified;
}
