/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/PujjoDmicProfile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put(PJ_BYTE *p, PJ_DWORD v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (PJ_BYTE)(v >> (8*i));
}
static void reject(const PJ_BYTE *p, PJ_SIZE n)
{
    pj_dmic_profile result, zero = {0};
    memset(&result, 0xa5, sizeof(result));
    assert(!pj_dmic_validate(p,n,&result));
    assert(!memcmp(&result,&zero,sizeof(result)));
}
static PJ_SIZE make(PJ_BYTE *data)
{
    const PJ_DWORD header[] = {
        1,0xffff1010,0xffffffff,0xffffffff,0xffffffff,16,3,0x3a0003,0,3
    };
    for (unsigned i=0; i<10; ++i) put(data+4*i,header[i]);
    PJ_SIZE pos=40;
    for(unsigned c=0;c<2;++c) {
        /* Synthetic clock: 38.4MHz / 8 / 20 / 5 = 48kHz. No real FIR filter
         * design is implied by the dummy single nonzero coefficient. */
        put(data+pos,0xc001);
        put(data+pos+4,(8u<<24)|(19u<<8));
        put(data+pos+12,(6u<<8)|3);
        for(unsigned f=0;f<2;++f) {
            put(data+pos+32+32*f,0x91);
            put(data+pos+36+32*f,4u<<16);
        }
        pos+=96;
        put(data+pos,1); put(data+pos+4,0xfffff);
        pos+=8;
    }
    return pos;
}
int main(int argc,char **argv)
{
    /* Deliberately unaligned. */
    PJ_BYTE bytes[1025]={0}, bad[1025];
    PJ_BYTE *p=bytes+1, *b=bad+1;
    PJ_SIZE n=make(p);
    pj_dmic_profile result;
    assert(n==248 && pj_dmic_validate(p,n,&result));
    assert(result.bytes==n && result.clock_divider==8 && result.pdm_clock_hz==4800000);
    for(unsigned c=0;c<2;++c) {
        assert(result.cic_decimation[c]==20);
        for(unsigned f=0;f<2;++f) {
            assert(result.taps[c][f]==1 && result.fir_decimation[c][f]==5);
        }
    }
    reject(NULL,n);
    assert(!pj_dmic_validate(p,n,NULL));
    for(PJ_SIZE i=0;i<n;++i) reject(p,i);
    reject(p,n+4);
    reject(p,PJ_DMIC_MAX_PAYLOAD+1);
    /* Flip every header bit, then every reserved field. */
    for(unsigned i=0;i<40;++i)
        for(unsigned bit=0;bit<8;++bit) {
            memcpy(b,p,n); b[i]^=(PJ_BYTE)(1u<<bit); reject(b,n);
        }
    const unsigned reserved[]={48,56,60,64,68,80,84,88,92,96,100,
        112,116,120,124,128,132,152,160,164,168,172,184,188,192,196,200,204,
        216,220,224,228,232,236};
    for(unsigned i=0;i<sizeof(reserved)/sizeof(reserved[0]);++i) {
        memcpy(b,p,n); put(b+reserved[i],1); reject(b,n);
    }
    const unsigned bad_words[][2]={
        {40,0xc000},{44,0x08000000},{44,0x0f001300},{52,0x503},
        {52,0xf03},{52,0x603|4},{72,0x90},{76,0},
        {76,0x40900},{76,0x400ff},{104,0x90},{108,0x30000},
        {136,0},{140,0x100000},{144,0xc000},{156,0x803},{240,0},{244,0x100000}
    };
    /* Some bit-field combinations can be legitimate; each listed word
     * deliberately violates this profile. */
    for(unsigned i=0;i<sizeof(bad_words)/sizeof(bad_words[0]);++i) {
        memcpy(b,p,n); put(b+bad_words[i][0],bad_words[i][1]); reject(b,n);
    }
    if(argc==2) {
        PJ_BYTE reference[PJ_DMIC_MAX_PAYLOAD+1];
        FILE *f=fopen(argv[1],"rb");assert(f);
        n=fread(reference,1,sizeof(reference),f);
        assert(!ferror(f));fclose(f);
        assert(pj_dmic_validate(reference,n,&result));
        for(PJ_SIZE i=0;i<n;++i) reject(reference,i);
        assert(result.bytes==n);
        printf("PASS: independent ALSA reference %u bytes, PDM %u Hz, CIC %u/%u, taps %u/%u/%u/%u\n",
            (unsigned)result.bytes,(unsigned)result.pdm_clock_hz,
            (unsigned)result.cic_decimation[0],(unsigned)result.cic_decimation[1],
            (unsigned)result.taps[0][0],(unsigned)result.taps[0][1],
            (unsigned)result.taps[1][0],(unsigned)result.taps[1][1]);
    }
    puts("PASS: Pujjo DMIC0 serialized profile, bounds, routing, clock, FIR and failure-output checks");
    return 0;
}
