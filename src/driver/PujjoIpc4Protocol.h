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
#ifdef __cplusplus
}
#endif
#endif
