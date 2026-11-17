/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoRt5682sCodec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct mock {
    uint16_t words[65536];
    unsigned reads, writes, bad_wire;
    int fail_read, fail_write, drop_write, swap_identity;
    uint8_t last_tx[4];
} mock;

static int mock_read(void *user, const uint8_t *tx, size_t tlen,
                     uint8_t *rx, size_t rlen)
{
    mock *m=(mock*)user;
    uint16_t reg, v;
    if (tlen!=2 || rlen!=2) {m->bad_wire++;return -1;}
    if (m->fail_read) return -1;
    reg=(uint16_t)(((uint16_t)tx[0]<<8)|tx[1]);
    v=m->words[reg];
    if (reg==PJ5682S_DEVICE_ID_REGISTER && m->swap_identity)
        v=0x1234;
    rx[0]=(uint8_t)(v>>8);
    rx[1]=(uint8_t)v;
    m->reads++;
    return 0;
}
static int mock_write(void *user, const uint8_t *tx, size_t len)
{
    mock *m=(mock*)user;
    uint16_t reg, val;
    if (len!=4) {m->bad_wire++;return -1;}
    if (m->fail_write) return -1;
    memcpy(m->last_tx,tx,4);
    reg=(uint16_t)(((uint16_t)tx[0]<<8)|tx[1]);
    val=(uint16_t)(((uint16_t)tx[2]<<8)|tx[3]);
    if (!m->drop_write) m->words[reg]=val;
    m->writes++;
    return 0;
}
static pj5682s_bus test_bus(mock *m)
{
    pj5682s_bus b={0};
    b.context=m;b.write_then_read=mock_read;b.write_only=mock_write;
    return b;
}
static void init(mock *m)
{
    memset(m,0,sizeof(*m));
    m->words[PJ5682S_DEVICE_ID_REGISTER]=PJ5682S_EXPECTED_DEVICE_ID;
    m->words[PJ5682S_VENDOR_ID_REGISTER]=0x10ec;
    m->words[PJ5682S_VERSION_REGISTER]=0x0002;
}
int main(void)
{
    pj5682s_codec c={0}, empty={0};
    mock m;
    pj5682s_bus b;
    uint8_t address[2], word[4];
    uint16_t value=0xabcd;
    unsigned prior;
    init(&m);b=test_bus(&m);

    pj5682s_encode_address(0x12a9,address);
    assert(address[0]==0x12 && address[1]==0xa9);
    pj5682s_encode_write(0x1234,0xabcd,word);
    assert(word[0]==0x12 && word[1]==0x34 &&
           word[2]==0xab && word[3]==0xcd);
    assert(pj5682s_decode_value(word+2)==0xabcd);
    assert(pj5682s_decode_value(NULL)==0);
    assert(pj5682s_bind(NULL,&b)==PJ5682S_BAD_ARGUMENT);
    assert(pj5682s_bind(&c,NULL)==PJ5682S_BAD_ARGUMENT);
    assert(pj5682s_read_identity(&c,0xff,&value)==PJ5682S_NOT_IDENTIFIED);
    assert(pj5682s_identify(&c)==PJ5682S_INCORRECT_STATE);
    assert(pj5682s_bind(&c,&b)==PJ5682S_OK);
    assert(pj5682s_bind(&c,&b)==PJ5682S_INCORRECT_STATE);
    assert(pj5682s_read_identity(&c,0xff,&value)==PJ5682S_NOT_IDENTIFIED);
    assert(pj5682s_identify(&c)==PJ5682S_OK);
    assert(c.state==PJ5682S_IDENTIFIED);
    assert(c.verified_id==PJ5682S_EXPECTED_DEVICE_ID);
    assert(pj5682s_identify(&c)==PJ5682S_OK);
    prior=m.reads;
    assert(pj5682s_read_identity(&c,0x0008,&value)==PJ5682S_BAD_ARGUMENT);
    assert(prior==m.reads);
    assert(pj5682s_read_identity(&c,PJ5682S_VENDOR_ID_REGISTER,&value)==PJ5682S_OK);
    assert(value==0x10ec);
    assert(pj5682s_read_identity(&c,PJ5682S_VERSION_REGISTER,&value)==PJ5682S_OK);
    assert(value==0x0002);
    assert(pj5682s_suspend(&c)==PJ5682S_OK);
    assert(c.verified_id==0);
    assert(pj5682s_read_identity(&c,0xff,&value)==PJ5682S_SUSPENDED);
    assert(pj5682s_suspend(&c)==PJ5682S_INCORRECT_STATE);
    assert(pj5682s_resume(&c)==PJ5682S_OK);
    assert(pj5682s_read_identity(&c,0xff,&value)==PJ5682S_NOT_IDENTIFIED);
    assert(pj5682s_identify(&c)==PJ5682S_OK);
    assert(pj5682s_resume(&c)==PJ5682S_INCORRECT_STATE);

#ifndef PJ5682S_TEST_ONLY_WRITES
    prior=m.writes;
    assert(pj5682s_test_update_bits(&c,0x0200,0xffff,0x0012)==PJ5682S_WRITE_LOCKED);
    assert(m.writes==prior);
#else
    m.words[0x0200]=0xa05a;
    assert(pj5682s_test_update_bits(&c,0x0200,0x00f0,0x00b0)==PJ5682S_OK);
    assert(m.words[0x0200]==0xa0ba);
    assert(m.writes==1);
    assert(m.last_tx[0]==0x02 && m.last_tx[1]==0x00 &&
           m.last_tx[2]==0xa0 && m.last_tx[3]==0xba);
    assert(pj5682s_test_update_bits(&c,0x0200,0x00f0,0x00b0)==PJ5682S_OK);
    assert(m.writes==1);
    assert(pj5682s_test_update_bits(&c,0x0200,0,0)==PJ5682S_BAD_ARGUMENT);
    assert(pj5682s_test_update_bits(&c,0x0200,0x00f0,0xf000)==PJ5682S_BAD_ARGUMENT);
    assert(pj5682s_test_update_bits(&c,0xff,0xff,1)==PJ5682S_BAD_ARGUMENT);
    m.fail_write=1;
    assert(pj5682s_test_update_bits(&c,0x0200,0x00f0,0x0050)==PJ5682S_BUS_ERROR);
    assert(c.state==PJ5682S_FAILED && c.verified_id==0);
    m.fail_write=0;
    pj5682s_unbind(&c);
    assert(pj5682s_bind(&c,&b)==PJ5682S_OK);
    assert(pj5682s_identify(&c)==PJ5682S_OK);
    m.drop_write=1;
    assert(pj5682s_test_update_bits(&c,0x0200,0x00f0,0x0000)==PJ5682S_VERIFY_FAILED);
    assert(c.state==PJ5682S_FAILED && c.verified_id==0);
    m.drop_write=0;
#endif
    pj5682s_unbind(&c);
    assert(c.state==PJ5682S_CLOSED);
    assert(c.bus.write_then_read==NULL);
    assert(pj5682s_bind(&c,&b)==PJ5682S_OK);
    m.swap_identity=1;
    assert(pj5682s_identify(&c)==PJ5682S_WRONG_CHIP);
    assert(c.state==PJ5682S_FAILED && c.verified_id==0);
    assert(pj5682s_identify(&c)==PJ5682S_INCORRECT_STATE);
    pj5682s_unbind(&c);
    m.swap_identity=0;m.fail_read=1;
    assert(pj5682s_bind(&c,&b)==PJ5682S_OK);
    assert(pj5682s_identify(&c)==PJ5682S_BUS_ERROR);
    assert(c.state==PJ5682S_FAILED && c.verified_id==0);
    m.fail_read=0;
    pj5682s_unbind(&c);
    assert(pj5682s_bind(&c,&b)==PJ5682S_OK);
    assert(pj5682s_identify(&c)==PJ5682S_OK);
    m.swap_identity=1;
    assert(pj5682s_read_identity(&c,0xff,&value)==PJ5682S_WRONG_CHIP);
    assert(c.state==PJ5682S_FAILED && c.verified_id==0);
    pj5682s_unbind(&c);
    m.swap_identity=0;
    assert(pj5682s_bind(&empty,&b)==PJ5682S_OK);
    assert(pj5682s_identify(&empty)==PJ5682S_OK);
    for (unsigned cycle=0;cycle<1000;++cycle) {
        assert(pj5682s_suspend(&empty)==PJ5682S_OK);
        assert(empty.verified_id==0);
        assert(pj5682s_resume(&empty)==PJ5682S_OK);
        assert(pj5682s_identify(&empty)==PJ5682S_OK);
    }
    assert(m.bad_wire==0);
    puts("PASS: RT5682S codec ID, I2C protocol, read guard, suspend, bus failures and mock-only write tests");
    return 0;
}
