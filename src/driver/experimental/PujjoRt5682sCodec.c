/* SPDX-License-Identifier: BSD-3-Clause */
/* New independent RT5682S codec-bus code: no GPL Linux code copied. */
#include "PujjoRt5682sCodec.h"
#if defined(PJ5682S_TEST_ONLY_WRITES) && defined(_KERNEL_MODE)
#error PJ5682S_TEST_ONLY_WRITES is forbidden in a Windows kernel build
#endif
#ifndef _KERNEL_MODE
#include <string.h>
#endif

void pj5682s_encode_address(PJ_WORD reg, PJ_BYTE out[2])
{
    if (!out) return;
    out[0]=(PJ_BYTE)(reg>>8);
    out[1]=(PJ_BYTE)reg;
}
void pj5682s_encode_write(PJ_WORD reg, PJ_WORD value, PJ_BYTE out[4])
{
    if (!out) return;
    out[0]=(PJ_BYTE)(reg>>8);
    out[1]=(PJ_BYTE)reg;
    out[2]=(PJ_BYTE)(value>>8);
    out[3]=(PJ_BYTE)value;
}
PJ_WORD pj5682s_decode_value(const PJ_BYTE bytes[2])
{
    if (!bytes) return 0;
    return (PJ_WORD)(((PJ_WORD)bytes[0]<<8)|bytes[1]);
}

pj5682s_status pj5682s_bind(pj5682s_codec *c, const pj5682s_bus *bus)
{
    if (!c || !bus || !bus->write_then_read) return PJ5682S_BAD_ARGUMENT;
    /* Refuse overwriting an existing live context. Caller must explicitly
     * unbind before reusing it; c must be initially zero-initialized. */
    if (c->state != PJ5682S_CLOSED) return PJ5682S_INCORRECT_STATE;
    c->bus=*bus;
    c->verified_id=0;
    c->successful_read_count=0;
    c->successful_test_write_count=0;
    c->state=PJ5682S_BOUND;
    return PJ5682S_OK;
}

static pj5682s_status read_raw(pj5682s_codec *c, PJ_WORD reg, PJ_WORD *out)
{
    PJ_BYTE addr[2], response[2]={0};
    if (!c || !out) return PJ5682S_BAD_ARGUMENT;
    if (!c->bus.write_then_read) return PJ5682S_NO_BUS;
    if (c->state==PJ5682S_ASLEEP) return PJ5682S_SUSPENDED;
    if (c->state==PJ5682S_FAILED) return PJ5682S_FAULT;
    if (c->state!=PJ5682S_BOUND && c->state!=PJ5682S_IDENTIFIED)
        return PJ5682S_NOT_IDENTIFIED;
    pj5682s_encode_address(reg, addr);
    if (c->bus.write_then_read(c->bus.context, addr, sizeof(addr),
                               response, sizeof(response)) != 0) {
        c->state=PJ5682S_FAILED;
        c->verified_id=0;
        return PJ5682S_BUS_ERROR;
    }
    *out=pj5682s_decode_value(response);
    ++c->successful_read_count;
    return PJ5682S_OK;
}

pj5682s_status pj5682s_identify(pj5682s_codec *c)
{
    PJ_WORD value=0;
    pj5682s_status s;
    if (!c) return PJ5682S_BAD_ARGUMENT;
    if (c->state==PJ5682S_IDENTIFIED) return PJ5682S_OK;
    if (c->state!=PJ5682S_BOUND) return PJ5682S_INCORRECT_STATE;
    s=read_raw(c, PJ5682S_DEVICE_ID_REGISTER, &value);
    if (s!=PJ5682S_OK) return s;
    if (value!=PJ5682S_EXPECTED_DEVICE_ID) {
        c->state=PJ5682S_FAILED;
        return PJ5682S_WRONG_CHIP;
    }
    c->verified_id=value;
    c->state=PJ5682S_IDENTIFIED;
    return PJ5682S_OK;
}

pj5682s_status pj5682s_read_identity(pj5682s_codec *c,
                                      PJ_WORD reg, PJ_WORD *out)
{
    PJ_WORD observed;
    pj5682s_status s;
    if (!c || !out) return PJ5682S_BAD_ARGUMENT;
    if (c->state!=PJ5682S_IDENTIFIED)
        return c->state==PJ5682S_ASLEEP ? PJ5682S_SUSPENDED :
            (c->state==PJ5682S_FAILED ? PJ5682S_FAULT : PJ5682S_NOT_IDENTIFIED);
    if (reg!=PJ5682S_DEVICE_ID_REGISTER &&
        reg!=PJ5682S_VENDOR_ID_REGISTER &&
        reg!=PJ5682S_VERSION_REGISTER) return PJ5682S_BAD_ARGUMENT;
    s=read_raw(c, reg, &observed);
    /* Device ID unexpectedly changes: treat identity as invalid permanently. */
    if (s==PJ5682S_OK && reg==PJ5682S_DEVICE_ID_REGISTER &&
        observed!=PJ5682S_EXPECTED_DEVICE_ID) {
        c->state=PJ5682S_FAILED;
        c->verified_id=0;
        return PJ5682S_WRONG_CHIP;
    }
    if (s==PJ5682S_OK) *out=observed;
    return s;
}

pj5682s_status pj5682s_collect_identity(pj5682s_codec *c, pj5682s_identity *out)
{
    pj5682s_identity result={0};
    pj5682s_status status;
    if (!c || !out) return PJ5682S_BAD_ARGUMENT;
    status=pj5682s_identify(c);
    if (status!=PJ5682S_OK) return status;
    status=pj5682s_read_identity(c, PJ5682S_VENDOR_ID_REGISTER, &result.vendor_id);
    if (status!=PJ5682S_OK) return status;
    status=pj5682s_read_identity(c, PJ5682S_VERSION_REGISTER, &result.version);
    if (status!=PJ5682S_OK) return status;
    status=pj5682s_read_identity(c, PJ5682S_DEVICE_ID_REGISTER, &result.device_id);
    if (status!=PJ5682S_OK) return status;
    *out=result;
    return PJ5682S_OK;
}

pj5682s_status pj5682s_suspend(pj5682s_codec *c)
{
    if (!c) return PJ5682S_BAD_ARGUMENT;
    if (c->state==PJ5682S_FAILED) return PJ5682S_FAULT;
    if (c->state!=PJ5682S_BOUND && c->state!=PJ5682S_IDENTIFIED)
        return PJ5682S_INCORRECT_STATE;
    c->verified_id=0;
    c->state=PJ5682S_ASLEEP;
    return PJ5682S_OK;
}
pj5682s_status pj5682s_resume(pj5682s_codec *c)
{
    if (!c) return PJ5682S_BAD_ARGUMENT;
    if (c->state!=PJ5682S_ASLEEP) return PJ5682S_INCORRECT_STATE;
    /* Wake is NOT a power sequence. Require re-identification of device. */
    c->state=PJ5682S_BOUND;
    return PJ5682S_OK;
}
void pj5682s_unbind(pj5682s_codec *c)
{
#ifdef _KERNEL_MODE
    if (c) RtlZeroMemory(c, sizeof(*c));
#else
    if (c) memset(c, 0, sizeof(*c));
#endif
}

pj5682s_status pj5682s_test_update_bits(pj5682s_codec *c,
        PJ_WORD reg, PJ_WORD mask, PJ_WORD value)
{
#ifdef PJ5682S_TEST_ONLY_WRITES
    PJ_WORD old_value=0, new_value=0, observed=0;
    PJ_BYTE tx[4];
    pj5682s_status status;
    if (!c || !mask || (value & (PJ_WORD)~mask)) return PJ5682S_BAD_ARGUMENT;
    if (c->state!=PJ5682S_IDENTIFIED) return PJ5682S_NOT_IDENTIFIED;
    if (!c->bus.write_only) return PJ5682S_NO_BUS;
    if (reg==PJ5682S_DEVICE_ID_REGISTER || reg==PJ5682S_VENDOR_ID_REGISTER ||
        reg==PJ5682S_VERSION_REGISTER) return PJ5682S_BAD_ARGUMENT;
    status=read_raw(c,reg,&old_value);
    if (status!=PJ5682S_OK) return status;
    new_value=(PJ_WORD)((old_value & (PJ_WORD)~mask)|value);
    if (new_value==old_value) return PJ5682S_OK;
    pj5682s_encode_write(reg,new_value,tx);
    if (c->bus.write_only(c->bus.context,tx,sizeof(tx))!=0) {
        c->state=PJ5682S_FAILED;
        c->verified_id=0;
        return PJ5682S_BUS_ERROR;
    }
    ++c->successful_test_write_count;
    /* Verification is only appropriate for nonvolatile mock registers;
     * production code must maintain a writable/volatile-register policy. */
    status=read_raw(c,reg,&observed);
    if (status!=PJ5682S_OK) return status;
    if ((observed & mask)!=(new_value & mask)) {
        c->state=PJ5682S_FAILED;
        c->verified_id=0;
        return PJ5682S_VERIFY_FAILED;
    }
    return PJ5682S_OK;
#else
    (void)c;(void)reg;(void)mask;(void)value;
    return PJ5682S_WRITE_LOCKED;
#endif
}
