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
    assert(pj_ipc4_mmio_span_valid(4096u,0u,4u));
    assert(pj_ipc4_mmio_span_valid(4096u,4092u,4u));
    assert(!pj_ipc4_mmio_span_valid(4096u,4092u,8u));
    assert(!pj_ipc4_mmio_span_valid(4096u,4096u,4u));
    assert(!pj_ipc4_mmio_span_valid(4096u,1u,4u));
    assert(!pj_ipc4_mmio_span_valid(4096u,4u,1u));
    assert(!pj_ipc4_mmio_span_valid(4096u,0u,0u));
    assert(!pj_ipc4_mmio_span_valid(0xffffffffu,0xfffffffcu,8u));
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
static void graph_tests(void)
{
    PJ_BYTE data[4096]={0};
    PJ_BYTE frame[4104]={0};
    PJ_SIZE n=0u;
    pj_ipc4_packet p={0}, q={0};
    assert(pj_ipc4_pipeline_create(3u,5u,256u,2u,1u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x11032900u && p.extension==0x00200001u);
    assert(p.payload==NULL && p.payload_bytes==0u);
    assert(pj_ipc4_pipeline_create(255u,31u,2047u,15u,1u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x11ffffffu && p.extension==0x00f00001u);
    assert(pj_ipc4_pipeline_create(256u,0u,0u,0u,0u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(p.primary==0x11ffffffu);
    assert(pj_ipc4_pipeline_create(0u,32u,0u,0u,0u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_create(0u,0u,2048u,0u,0u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_create(0u,0u,0u,16u,0u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_create(0u,0u,0u,0u,2u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_delete(3u,&p)==PJ_IPC4_VALID && p.primary==0x12030000u);
    assert(p.extension==0u && p.payload_bytes==0u);
    assert(pj_ipc4_pipeline_delete(256u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_state(3u,4u,&p)==PJ_IPC4_VALID && p.primary==0x13030004u);
    assert(pj_ipc4_pipeline_state(3u,1u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_state(3u,6u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_state(256u,4u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_pipeline_state(3u,3u,NULL)==PJ_IPC4_BAD_PARAMETER);
    data[0]=0x5au;
    assert(pj_ipc4_module_init(0x1234u,2u,3u,1u,1u,data,16u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x40021234u && p.extension==0x11030004u);
    assert(p.payload==data && p.payload_bytes==16u);
    assert(pj_ipc4_module_init(65536u,2u,3u,1u,0u,data,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,256u,3u,1u,0u,data,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,256u,1u,0u,data,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,16u,0u,data,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,2u,data,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,0u,NULL,16u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,0u,data,0u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,0u,data,15u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,0u,data,4100u,&p)==PJ_IPC4_TOO_LARGE);
    assert(pj_ipc4_module_init(1u,2u,3u,1u,0u,data,sizeof(data),&p)==PJ_IPC4_VALID);
    assert((p.extension&0xffffu)==1024u);
    assert(pj_ipc4_encode(&p,frame,sizeof(frame),&n)==PJ_IPC4_VALID && n==sizeof(frame));
    assert(pj_ipc4_decode(frame,n,&q)==PJ_IPC4_VALID && q.payload_bytes==sizeof(data));
    assert(q.primary==p.primary && q.extension==p.extension && q.payload[0]==0x5au);
    assert(pj_ipc4_module_link(1u,0x1234u,2u,3u,0x5678u,4u,5u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x45021234u && p.extension==0x1d045678u);
    assert(pj_ipc4_module_link(0u,0x1234u,2u,3u,0x5678u,4u,5u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x46021234u && p.extension==0x1d045678u);
    assert(p.payload==NULL && p.payload_bytes==0u);
    assert(pj_ipc4_module_link(1u,65535u,255u,7u,65535u,255u,7u,&p)==PJ_IPC4_VALID);
    assert(p.primary==0x45ffffffu && p.extension==0x3fffffffu);
    assert(pj_ipc4_module_link(2u,1u,2u,3u,4u,5u,6u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_link(1u,1u,2u,8u,4u,5u,6u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(pj_ipc4_module_link(1u,1u,2u,3u,4u,5u,8u,&p)==PJ_IPC4_BAD_PARAMETER);
    assert(p.primary==0x45ffffffu && p.extension==0x3fffffffu);
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
    protocol_tests();graph_tests();transaction_tests();
    puts("PASS: IPC4 protocol frames and fail-closed transaction model");
    return 0;
}
