/* SPDX-License-Identifier: BSD-3-Clause */
/* Pure protocol decoder and host transaction tests: no chipset operations. */
#include "../src/driver/PujjoIpc4Protocol.h"
#include "../src/driver/PujjoIpc4Transaction.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void protocol_tests(void)
{
    PJ_BYTE out[20]={0}, malformed[12]={0}, payload[5]={2u,3u,5u,7u,11u};
    pj_ipc4_packet p={0x02000010u,0xaabbccddu,payload,sizeof(payload)},decoded={0};
    PJ_SIZE n=123u;
    assert(pj_ipc4_validate(NULL)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_validate(&p)==PJ_IPC4_VALID);
    assert(pj_ipc4_encode(&p,out,sizeof(out),&n)==PJ_IPC4_VALID && n==13u);
    assert(out[0]==0x10u && out[3]==0x02u && out[4]==0xddu && out[7]==0xaau);
    assert(memcmp(out+8u,payload,5u)==0);
    assert(pj_ipc4_decode(out,n,&decoded)==PJ_IPC4_VALID);
    assert(decoded.primary==p.primary && decoded.extension==p.extension && decoded.payload_bytes==5u);
    assert(memcmp(decoded.payload,payload,5u)==0);
    assert(pj_ipc4_decode(NULL,n,&decoded)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_decode(out,7u,&decoded)==PJ_IPC4_TRUNCATED);
    assert(pj_ipc4_encode(&p,out,12u,&n)==PJ_IPC4_BUFFER_SMALL && n==0u);
    assert(pj_ipc4_encode(&p,NULL,sizeof(out),&n)==PJ_IPC4_BAD_PARAMETER);
    p.payload=NULL;
    assert(pj_ipc4_encode(&p,out,sizeof(out),&n)==PJ_IPC4_BAD_PARAMETER);
    p.payload=payload;
    p.payload_bytes=4097u;
    assert(pj_ipc4_validate(&p)==PJ_IPC4_TOO_LARGE);
    p.payload_bytes=5u;p.primary|=PJ_IPC4_RESERVED_MASK;
    assert(pj_ipc4_validate(&p)==PJ_IPC4_INVALID_HEADER);
    p.primary=0x02000010u;
    assert(pj_ipc4_decode(malformed,sizeof(malformed),&decoded)==PJ_IPC4_VALID);
    malformed[3]=0x80u;
    assert(pj_ipc4_decode(malformed,sizeof(malformed),&decoded)==PJ_IPC4_INVALID_HEADER);
    assert(pj_ipc4_is_fw_ready(0x1b080000u));
    assert(!pj_ipc4_is_fw_ready(0x1b070000u));
    assert(!pj_ipc4_is_fw_ready(0x1b080000u|PJ_IPC4_TARGET_MASK));
    assert(!pj_ipc4_is_fw_ready(0x1b080000u|PJ_IPC4_DIRECTION_MASK));
    assert(!pj_ipc4_is_fw_ready(0x1b080000u|PJ_IPC4_RESERVED_MASK));
    assert(pj_ipc4_reply_matches(0x02000010u,0x22000000u));
    assert(!pj_ipc4_reply_matches(0x02000010u,0x23000000u));
    assert(!pj_ipc4_reply_matches(0x02000010u,0x02000000u));
    assert(!pj_ipc4_reply_matches(0x02000010u,0x62000000u));
    assert(!pj_ipc4_reply_matches(0x02000010u,0xa2000000u));
    assert(!pj_ipc4_reply_matches(0x1b000000u,0x3b000000u));
    /* Worst-case valid payload boundary must not be misclassified. */
    {
        PJ_BYTE big[4105]={0};
        assert(pj_ipc4_decode(big,4104u,&decoded)==PJ_IPC4_VALID);
        assert(pj_ipc4_decode(big,4105u,&decoded)==PJ_IPC4_TOO_LARGE);
    }
    {
        unsigned i;
        /* Malformed shorter frames are always rejected, never dereferenced. */
        for(i=0u;i<8u;++i) assert(pj_ipc4_decode(out,(PJ_SIZE)i,&decoded)==PJ_IPC4_TRUNCATED);
    }
}
static void transaction_tests(void)
{
    pj_ipc4_transaction t;
    PJ_DWORD token=99u;
#ifdef PJ_IPC4_TRANSACTION_HOST_TEST
    PJ_DWORD second=0u;
#endif
    pj_tx_init(&t);
    assert(t.state==PJ_TX_IDLE && !pj_tx_is_complete(&t));
    assert(pj_tx_start(NULL,0x02000000u,25u,&token)==PJ_TX_BAD_ARGUMENT);
    assert(pj_tx_start(&t,0x02000000u,0u,&token)==PJ_TX_BAD_ARGUMENT);
    assert(pj_tx_start(&t,0x82000000u,25u,&token)==PJ_TX_BAD_ARGUMENT);
    assert(pj_tx_start(&t,0x22000000u,25u,&token)==PJ_TX_BAD_ARGUMENT);
    assert(pj_tx_start(&t,0x1b080000u,25u,&token)==PJ_TX_BAD_ARGUMENT);
#ifndef PJ_IPC4_TRANSACTION_HOST_TEST
    assert(pj_tx_start(&t,0x02000000u,25u,&token)==PJ_TX_LOCKED);
    assert(token==0u && t.state==PJ_TX_IDLE);
    assert(pj_tx_reset(&t)==PJ_TX_LOCKED);
#else
    assert(pj_tx_start(&t,0x02000000u,25u,&token)==PJ_TX_OK);
    assert(token!=0u && t.state==PJ_TX_WAIT_REPLY);
    assert(pj_tx_start(&t,0x02000000u,25u,&second)==PJ_TX_BUSY);
    assert(second==0u);
    assert(pj_tx_receive(&t,token-1u,0x22000000u)==PJ_TX_STALE);
    assert(pj_tx_receive(&t,token,0x23000000u)==PJ_TX_UNEXPECTED);
    assert(pj_tx_receive(&t,token,0x02000000u)==PJ_TX_UNEXPECTED);
    assert(pj_tx_elapsed(&t,24u)==PJ_TX_OK);
    assert(pj_tx_receive(&t,token,0x22000000u)==PJ_TX_OK);
    assert(t.state==PJ_TX_COMPLETE && pj_tx_is_complete(&t));
    assert(pj_tx_receive(&t,token,0x22000000u)==PJ_TX_UNEXPECTED);
    assert(pj_tx_start(&t,0x02000000u,25u,&second)==PJ_TX_BUSY);
    assert(pj_tx_reset(&t)==PJ_TX_OK && t.state==PJ_TX_IDLE);
    assert(pj_tx_start(&t,0x02000000u,25u,&second)==PJ_TX_OK);
    assert(second!=token);
    assert(pj_tx_receive(&t,token,0x22000000u)==PJ_TX_STALE);
    assert(pj_tx_elapsed(&t,25u)==PJ_TX_TIMEOUT);
    assert(t.state==PJ_TX_FAULTED && !pj_tx_is_complete(&t));
    assert(pj_tx_receive(&t,second,0x22000000u)==PJ_TX_FAULT);
    assert(pj_tx_start(&t,0x02000000u,25u,&token)==PJ_TX_FAULT);
    assert(pj_tx_reset(&t)==PJ_TX_OK && t.state==PJ_TX_IDLE);
    t.generation=0xffffffffu;
    assert(pj_tx_start(&t,0x02000000u,25u,&token)==PJ_TX_FAULT);
    assert(t.state==PJ_TX_FAULTED);
#endif
}
int main(void)
{
    protocol_tests();transaction_tests();
    puts("PASS: IPC4 protocol frames and fail-closed transaction model");
    return 0;
}
