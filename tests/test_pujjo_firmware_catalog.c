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
static void wr32(PJ_BYTE *p,PJ_DWORD n) {
    p[0]=(PJ_BYTE)n; p[1]=(PJ_BYTE)(n>>8);
    p[2]=(PJ_BYTE)(n>>16); p[3]=(PJ_BYTE)(n>>24);
}
static void wr16(PJ_BYTE *p,PJ_WORD n) {
    p[0]=(PJ_BYTE)n; p[1]=(PJ_BYTE)(n>>8);
}
int main(void) {
    PJ_BYTE image[C1+44u]={0}, uuid[16]={1u,2u,3u}, absent[16]={5u};
    pj_firmware_module m={0};
    pj_firmware_config c={0};
    wr32(image,0x31454124u); wr32(image+4u,EXT);
    wr32(image+FW,0x314d4124u); wr32(image+FW+4u,52u);
    wr32(image+FW+36u,2u);
    wr32(image+M0,0x454d4124u); wr32(image+M1,0x454d4124u);
    memcpy(image+M1+12u,uuid,16u);
    wr16(image+M1+70u,2u); wr32(image+M1+72u,1u);
    wr16(image+M1+76u,12u);
    wr32(image+C0+16u,280u); wr32(image+C0+24u,384u);
    wr32(image+C0+28u,384u); wr32(image+C0+36u,7363u);
    wr32(image+C1+16u,512u); wr32(image+C1+24u,192u);
    wr32(image+C1+28u,192u); wr32(image+C1+36u,4000u);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,&m)==PJ_CATALOG_OK);
    assert(m.module_id==1u && m.instance_max_count==12u && m.affinity_mask==1u);
    assert(m.config_count==2u && memcmp(m.uuid,uuid,16u)==0);
    assert(pj_firmware_find_config(image,sizeof(image),uuid,384u,384u,&c)==PJ_CATALOG_OK);
    assert(c.instance_bytes==280u && c.cpc==7363u && c.ibs==384u && c.obs==384u);
    assert(pj_firmware_find_config(image,sizeof(image),uuid,192u,192u,&c)==PJ_CATALOG_OK);
    assert(c.instance_bytes==512u && c.cpc==4000u);
    assert(pj_firmware_find_config(image,sizeof(image),uuid,123u,123u,&c)==PJ_CATALOG_NOT_FOUND);
    assert(c.cpc==4000u);
    assert(pj_firmware_find_module(image,sizeof(image),absent,&m)==PJ_CATALOG_NOT_FOUND);
    assert(m.module_id==1u);
    assert(pj_firmware_find_module(image,C1,uuid,&m)==PJ_CATALOG_INVALID_IMAGE);
    assert(pj_firmware_find_module(NULL,0u,uuid,&m)==PJ_CATALOG_INVALID_ARGUMENT);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,NULL)==PJ_CATALOG_INVALID_ARGUMENT);
    memcpy(image+M0+12u,uuid,16u);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,&m)==PJ_CATALOG_AMBIGUOUS);
    memset(image+M0+12u,0,16u);
    wr32(image+C1+24u,384u); wr32(image+C1+28u,384u);
    assert(pj_firmware_find_config(image,sizeof(image),uuid,384u,384u,&c)==PJ_CATALOG_AMBIGUOUS);
    wr32(image+C1+16u,280u); wr32(image+C1+36u,7363u);
    assert(pj_firmware_find_config(image,sizeof(image),uuid,384u,384u,&c)==PJ_CATALOG_OK);
    wr16(image+M1+68u,65535u);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,&m)==PJ_CATALOG_INVALID_IMAGE);
    wr16(image+M1+68u,0u);
    wr32(image+FW+36u,0xffffffffu);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,&m)==PJ_CATALOG_INVALID_IMAGE);
    wr32(image+FW+36u,2u);
    wr32(image+4u,0xffffffffu);
    assert(pj_firmware_find_module(image,sizeof(image),uuid,&m)==PJ_CATALOG_INVALID_IMAGE);
    assert(m.module_id==1u);
    puts("PASS: UUID module identity, configuration matching, malformed/ambiguous manifest rejection");
    return 0;
}
