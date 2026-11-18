/* SPDX-License-Identifier: BSD-3-Clause */
/* IPC4 protocol-only host/WDK routines. No hardware access or transport. */
#ifndef PUJJO_IPC4_PROTOCOL_H
#define PUJJO_IPC4_PROTOCOL_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PJ_IPC4_HEADER_BYTES 8u
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
#define PJ_IPC4_DIRECTION_MASK 0x20000000u
#define PJ_IPC4_TARGET_MASK 0x40000000u
#define PJ_IPC4_RESERVED_MASK 0x80000000u
#define PJ_IPC4_TYPE_MASK 0x1f000000u
#define PJ_IPC4_CORRELATION_MASK 0x5f000000u
#define PJ_IPC4_GLOBAL_NOTIFICATION 27u
#define PJ_IPC4_NOTIFY_FW_READY 8u

typedef enum {
    PJ_IPC4_VALID = 0, PJ_IPC4_BAD_PARAMETER = -1,
    PJ_IPC4_TRUNCATED = -2, PJ_IPC4_TOO_LARGE = -3,
    PJ_IPC4_INVALID_HEADER = -4, PJ_IPC4_BUFFER_SMALL = -5
} pj_ipc4_status;
typedef struct {
    PJ_DWORD primary;
    PJ_DWORD extension;
    const PJ_BYTE *payload;
    PJ_SIZE payload_bytes;
} pj_ipc4_packet;

/* Wire representation of two little-endian header DWORDs plus optional bytes.
 * Generic mailbox framing ONLY: this is NOT the DSP doorbell format or a
 * complete IPC4 command/response serializer. Caller owns payload memory. */
pj_ipc4_status pj_ipc4_decode(const PJ_BYTE *input, PJ_SIZE input_bytes,
                               pj_ipc4_packet *packet);
pj_ipc4_status pj_ipc4_encode(const pj_ipc4_packet *packet,
                               PJ_BYTE *output, PJ_SIZE output_bytes,
                               PJ_SIZE *written);
pj_ipc4_status pj_ipc4_validate(const pj_ipc4_packet *packet);
int pj_ipc4_is_reply(PJ_DWORD primary);
int pj_ipc4_is_notification(PJ_DWORD primary);
int pj_ipc4_is_fw_ready(PJ_DWORD primary);
int pj_ipc4_reply_matches(PJ_DWORD request, PJ_DWORD reply);
/* IPC4 pipeline/module command builders. Header fields follow the bundled
 * dual BSD/GPL Intel IPC4 header; no transport or hardware access.
 * Module-init payload must be caller-owned, firmware-specific and DWORD aligned
 * in length. Output is unchanged on failure. These do not allocate DSP IDs. */
pj_ipc4_status pj_ipc4_pipeline_create(PJ_DWORD id, PJ_DWORD priority,
    PJ_DWORD memory_pages, PJ_DWORD core, PJ_DWORD low_power, pj_ipc4_packet *out);
pj_ipc4_status pj_ipc4_pipeline_delete(PJ_DWORD id, pj_ipc4_packet *out);
pj_ipc4_status pj_ipc4_pipeline_state(PJ_DWORD id, PJ_DWORD state, pj_ipc4_packet *out);
pj_ipc4_status pj_ipc4_module_init(PJ_DWORD module, PJ_DWORD instance,
    PJ_DWORD pipeline, PJ_DWORD core, PJ_DWORD domain,
    const PJ_BYTE *payload, PJ_SIZE bytes, pj_ipc4_packet *out);
pj_ipc4_status pj_ipc4_module_link(PJ_DWORD bind, PJ_DWORD src_module,
    PJ_DWORD src_instance, PJ_DWORD src_queue, PJ_DWORD dst_module,
    PJ_DWORD dst_instance, PJ_DWORD dst_queue, pj_ipc4_packet *out);

/* DWORD-aligned span inside an already mapped MMIO window; no I/O. */
int pj_ipc4_mmio_span_valid(PJ_DWORD mapped_bytes, PJ_DWORD offset, PJ_DWORD bytes);
#ifdef __cplusplus
}
#endif
#endif
