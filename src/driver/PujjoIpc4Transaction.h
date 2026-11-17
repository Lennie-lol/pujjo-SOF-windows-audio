/* SPDX-License-Identifier: BSD-3-Clause */
/* Pure IPC4 logical state machine. Caller serializes access. No device I/O.
 * Generation numbers are host-side markers, NOT transmitted IPC4 sequence IDs.
 * They cannot distinguish physically delayed untagged replies by themselves. */
#ifndef PUJJO_IPC4_TRANSACTION_H
#define PUJJO_IPC4_TRANSACTION_H
#include "PujjoIpc4Protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PJ_TX_OK=0, PJ_TX_BAD_ARGUMENT=-1, PJ_TX_BUSY=-2,
    PJ_TX_UNEXPECTED=-3, PJ_TX_STALE=-4, PJ_TX_TIMEOUT=-5,
    PJ_TX_FAULT=-6, PJ_TX_LOCKED=-7
} pj_tx_result;
typedef enum {
    PJ_TX_IDLE=0, PJ_TX_WAIT_REPLY, PJ_TX_COMPLETE, PJ_TX_FAULTED
} pj_tx_state;
typedef struct {
    pj_tx_state state;
    PJ_DWORD generation;
    PJ_DWORD request_primary;
    PJ_DWORD reply_primary;
    PJ_DWORD remaining_ms;
} pj_ipc4_transaction;
void pj_tx_init(pj_ipc4_transaction *t);
/* Always locked without explicit host-model opt-in; intentionally not wired
 * to hardware. `token` must match on completion (host-test boundary only). */
pj_tx_result pj_tx_start(pj_ipc4_transaction *t, PJ_DWORD request,
                           PJ_DWORD timeout_ms, PJ_DWORD *token);
pj_tx_result pj_tx_receive(pj_ipc4_transaction *t, PJ_DWORD token,
                             PJ_DWORD reply);
pj_tx_result pj_tx_elapsed(pj_ipc4_transaction *t, PJ_DWORD milliseconds);
pj_tx_result pj_tx_reset(pj_ipc4_transaction *t);
int pj_tx_is_complete(const pj_ipc4_transaction *t);
#ifdef __cplusplus
}
#endif
#endif
