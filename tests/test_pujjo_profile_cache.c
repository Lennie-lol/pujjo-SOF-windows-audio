/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/PujjoFirmwareCatalog.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define EXT 16u
#define FW (EXT+0x2000u)
#define M0 (FW+52u)
#define M1 (M0+116u)
#define C0 (M1+116u)
#define C1 (C0+44u)
static void wr32(PJ_BYTE *p,PJ_DWORD n) {for(unsigned i=0;i<4u;++i)p[i]=(PJ_BYTE)(n>>(8u*i));}
static void wr16(PJ_BYTE *p,PJ_WORD n) {p[0]=(PJ_BYTE)n;p[1]=(PJ_BYTE)(n>>8);}
static void record(PJ_BYTE *p,PJ_DWORD size,PJ_DWORD ibs,PJ_DWORD obs,PJ_DWORD cpc){wr32(p+16u,size);wr32(p+24u,ibs);wr32(p+28u,obs);wr32(p+36u,cpc);}
int main(void) {
    PJ_BYTE image[C0+65u*44u]={0},uuid[16]={1u,2u,3u},missing[16]={3u};
    pj_firmware_profile p={0},previous;
    pj_firmware_config actual={0},expected={0},sentinel={11,22,33,44};
    wr32(image,0x31454124u);wr32(image+4u,EXT);wr32(image+FW,0x314d4124u);wr32(image+FW+4u,52u);wr32(image+FW+36u,2u);
    wr32(image+M0,0x454d4124u);wr32(image+M1,0x454d4124u);memcpy(image+M1+12u,uuid,16u);
    wr32(image+M1+28u,0x20u);
    wr16(image+M1+70u,2u);wr32(image+M1+72u,1u);wr16(image+M1+76u,12u);
    record(image+C0,280u,384u,384u,7363u);record(image+C1,512u,192u,192u,4000u);
    assert(pj_firmware_load_profile(image,sizeof(image),uuid,&p)==PJ_CATALOG_OK);
    assert(p.module.module_id==1u && p.module.instance_max_count==12u && p.count==2u);
    for(PJ_DWORD n=1;n<=512u;++n){
        actual=expected=sentinel;
        assert(pj_firmware_profile_config(&p,n,n,&actual)==pj_firmware_find_config(image,sizeof(image),uuid,n,n,&expected));
        assert(memcmp(&actual,&expected,sizeof(actual))==0);
    }
    previous=p;
    assert(pj_firmware_load_profile(image,sizeof(image),missing,&p)==PJ_CATALOG_NOT_FOUND);
    assert(memcmp(&p,&previous,sizeof(p))==0);
    assert(pj_firmware_load_profile(image,C1,uuid,&p)==PJ_CATALOG_INVALID_IMAGE);
    assert(memcmp(&p,&previous,sizeof(p))==0);
    wr16(image+M1+70u,65u);
    assert(pj_firmware_load_profile(image,sizeof(image),uuid,&p)==PJ_CATALOG_INVALID_IMAGE);
    assert(memcmp(&p,&previous,sizeof(p))==0);
    wr16(image+M1+70u,0u);
    assert(pj_firmware_load_profile(image,sizeof(image),uuid,&p)==PJ_CATALOG_NOT_FOUND);
    wr16(image+M1+70u,2u);
    record(image+C1,512u,384u,384u,4000u);
    assert(pj_firmware_load_profile(image,sizeof(image),uuid,&p)==PJ_CATALOG_OK);
    actual=sentinel;
    assert(pj_firmware_profile_config(&p,384u,384u,&actual)==PJ_CATALOG_AMBIGUOUS);
    assert(memcmp(&actual,&sentinel,sizeof(actual))==0);
    record(image+C1,280u,384u,384u,7363u);
    assert(pj_firmware_load_profile(image,sizeof(image),uuid,&p)==PJ_CATALOG_OK);
    memset(image,0,sizeof(image)); /* cached data survives firmware release */
    assert(pj_firmware_profile_config(&p,384u,384u,&actual)==PJ_CATALOG_OK && actual.cpc==7363u);
    PJ_DWORD pages=0;
    assert(pj_firmware_plan_ll(&p,384u,384u,&actual,&pages)==PJ_CATALOG_OK);
    assert(pages==2u && actual.cpc==7363u && actual.instance_bytes==280u);
    assert(pj_firmware_plan_ll(&p,192u,192u,&actual,&pages)==PJ_CATALOG_OK);
    assert(pages==1u && actual.cpc==0u && actual.instance_bytes==280u);
    p.configs[1].cpc=8000u;
    assert(pj_firmware_plan_ll(&p,384u,384u,&actual,&pages)==PJ_CATALOG_OK && actual.cpc==8000u);
    p.configs[1].instance_bytes=500u;
    assert(pj_firmware_plan_ll(&p,384u,384u,&actual,&pages)==PJ_CATALOG_AMBIGUOUS);
    assert(pj_firmware_plan_ll(&p,192u,192u,&actual,&pages)==PJ_CATALOG_NOT_FOUND);
    assert(pj_firmware_plan_ll(&p,10000u,10000u,&actual,&pages)==PJ_CATALOG_NOT_FOUND);
    assert(actual.cpc==8000u && pages==2u);
    actual=sentinel;p.count=65u;
    assert(pj_firmware_profile_config(&p,384u,384u,&actual)==PJ_CATALOG_INVALID_IMAGE);
    assert(memcmp(&actual,&sentinel,sizeof(actual))==0);
    p.count=1;
    assert(pj_firmware_profile_config(&p,384u,384u,&actual)==PJ_CATALOG_INVALID_IMAGE);
    assert(pj_firmware_profile_config(&p,0u,384u,&actual)==PJ_CATALOG_INVALID_ARGUMENT);
    assert(pj_firmware_profile_config(&p,384u,384u,NULL)==PJ_CATALOG_INVALID_ARGUMENT);
    puts("PASS: cached manifest profiles, exact runtime geometry, failure atomicity, capacity and ambiguity");
    return 0;
}
