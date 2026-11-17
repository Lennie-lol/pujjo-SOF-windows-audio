/* SPDX-License-Identifier: BSD-3-Clause */
#if defined(PJ_IPC4_TRANSACTION_HOST_TEST) && defined(_KERNEL_MODE)
#error "IPC4 transaction mock is forbidden in WDK kernels"
#endif
#include "PujjoIpc4Transaction.h"
void pj_tx_init(pj_ipc4_transaction *t)
{
    if (!t) return;
    t->state=PJ_TX_IDLE;
    t->generation=1u;
    t->request_primary=0u;
    t->reply_primary=0u;
    t->remaining_ms=0u;
}
pj_tx_result pj_tx_start(pj_ipc4_transaction *t, PJ_DWORD request,
                           PJ_DWORD timeout_ms, PJ_DWORD *token)
{
    if (!t || !token) return PJ_TX_BAD_ARGUMENT;
    *token=0u;
    if (!timeout_ms || timeout_ms>60000u || request & PJ_IPC4_RESERVED_MASK ||
        pj_ipc4_is_reply(request) || pj_ipc4_is_notification(request))
        return PJ_TX_BAD_ARGUMENT;
    if (t->state==PJ_TX_FAULTED) return PJ_TX_FAULT;
    if (t->state!=PJ_TX_IDLE) return PJ_TX_BUSY;
#ifndef PJ_IPC4_TRANSACTION_HOST_TEST
    return PJ_TX_LOCKED;
#else
    if (t->generation==0xffffffffu) {
        t->state=PJ_TX_FAULTED;
        return PJ_TX_FAULT;
    }
    ++t->generation;
    t->state=PJ_TX_WAIT_REPLY;
    t->request_primary=request;
    t->remaining_ms=timeout_ms;
    t->reply_primary=0u;
    *token=t->generation;
    return PJ_TX_OK;
#endif
}
pj_tx_result pj_tx_receive(pj_ipc4_transaction *t, PJ_DWORD token,
                             PJ_DWORD reply)
{
    if (!t) return PJ_TX_BAD_ARGUMENT;
    if (t->state==PJ_TX_FAULTED) return PJ_TX_FAULT;
    if (t->state!=PJ_TX_WAIT_REPLY) return PJ_TX_UNEXPECTED;
    if (!token || token!=t->generation) return PJ_TX_STALE;
    if (!pj_ipc4_reply_matches(t->request_primary,reply)) return PJ_TX_UNEXPECTED;
#ifndef PJ_IPC4_TRANSACTION_HOST_TEST
    return PJ_TX_LOCKED;
#else
    t->reply_primary=reply;
    t->state=PJ_TX_COMPLETE;
    t->remaining_ms=0u;
    return PJ_TX_OK;
#endif
}
pj_tx_result pj_tx_elapsed(pj_ipc4_transaction *t, PJ_DWORD milliseconds)
{
    if (!t) return PJ_TX_BAD_ARGUMENT;
    if (t->state==PJ_TX_FAULTED) return PJ_TX_FAULT;
    if (t->state!=PJ_TX_WAIT_REPLY) return PJ_TX_UNEXPECTED;
#ifndef PJ_IPC4_TRANSACTION_HOST_TEST
    (void)milliseconds;
    return PJ_TX_LOCKED;
#else
    if (milliseconds>=t->remaining_ms) {
        t->remaining_ms=0u;
        t->state=PJ_TX_FAULTED;
        return PJ_TX_TIMEOUT;
    }
    t->remaining_ms-=milliseconds;
    return PJ_TX_OK;
#endif
}
pj_tx_result pj_tx_reset(pj_ipc4_transaction *t)
{
    if (!t) return PJ_TX_BAD_ARGUMENT;
#ifndef PJ_IPC4_TRANSACTION_HOST_TEST
    return PJ_TX_LOCKED;
#else
    if (t->generation==0xffffffffu) {
        t->state=PJ_TX_FAULTED;
        return PJ_TX_FAULT;
    }
    ++t->generation; /* stale tokens invalid across reset/power cycles */
    t->state=PJ_TX_IDLE;
    t->request_primary=0u;
    t->reply_primary=0u;
    t->remaining_ms=0u;
    return PJ_TX_OK;
#endif
}
int pj_tx_is_complete(const pj_ipc4_transaction *t)
{
    return t && t->state==PJ_TX_COMPLETE;
}
