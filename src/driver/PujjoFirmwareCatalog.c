/* SPDX-License-Identifier: BSD-3-Clause */
/* Layout from the repository's dual-licensed include/sof/ext_manifest4.h:
 * extended header 16 bytes; base binary header 52; module entry 116;
 * module configuration 44. Decode byte-wise: no alignment/packing dependence.
 * Base firmware IDs are table indices (Linux SOF ipc4-loader.c). */
#include "PujjoFirmwareCatalog.h"
#ifdef _KERNEL_MODE
#include <sof/sof_osal.h>
#include <sof/ext_manifest4.h>
#endif
#define EXT_HEADER 16u
#define FW_OFFSET 0x2000u
#define FW_HEADER 52u
#define MODULE_BYTES 116u
#define CONFIG_BYTES 44u
#define MAX_BASE_MODULES 4096u
#ifdef _KERNEL_MODE
C_ASSERT(sizeof(struct sof_ext_manifest4_hdr) == EXT_HEADER);
C_ASSERT(sizeof(struct sof_man4_fw_binary_header) == FW_HEADER);
C_ASSERT(sizeof(struct sof_man4_module) == MODULE_BYTES);
C_ASSERT(sizeof(struct sof_man4_module_config) == CONFIG_BYTES);
C_ASSERT(FIELD_OFFSET(struct sof_man4_fw_binary_header, num_module_entries) == 36);
C_ASSERT(FIELD_OFFSET(struct sof_man4_module, uuid) == 12);
C_ASSERT(FIELD_OFFSET(struct sof_man4_module, cfg_offset) == 68);
C_ASSERT(FIELD_OFFSET(struct sof_man4_module, instance_max_count) == 76);
C_ASSERT(FIELD_OFFSET(struct sof_man4_module_config, cpc) == 36);
#endif
typedef struct {
    const PJ_BYTE *modules;
    const PJ_BYTE *configs;
    PJ_DWORD count;
    PJ_SIZE config_capacity;
} catalog_view;
static PJ_WORD rd16(const PJ_BYTE *p)
{
    return (PJ_WORD)((PJ_WORD)p[0] | ((PJ_WORD)p[1] << 8));
}
static PJ_DWORD rd32(const PJ_BYTE *p)
{
    return (PJ_DWORD)p[0] | ((PJ_DWORD)p[1] << 8) |
        ((PJ_DWORD)p[2] << 16) | ((PJ_DWORD)p[3] << 24);
}
static int uuid_equal(const PJ_BYTE *a, const PJ_BYTE *b)
{
    PJ_DWORD i;
    for (i=0u;i<16u;++i) if (a[i]!=b[i]) return 0;
    return 1;
}
static int uuid_nonzero(const PJ_BYTE *u)
{
    PJ_DWORD i;
    for (i=0u;i<16u;++i) if (u[i]!=0u) return 1;
    return 0;
}
static pj_catalog_status parse(const PJ_BYTE *image, PJ_SIZE size, catalog_view *v)
{
    PJ_SIZE ext, remaining, header, table;
    PJ_DWORD i;
    const PJ_BYTE *fw;
    if (!image || size < EXT_HEADER || size > (PJ_SIZE)0xffffffffu ||
        rd32(image)!=0x31454124u) return PJ_CATALOG_INVALID_IMAGE;
    ext=rd32(image+4u);
    if (ext<EXT_HEADER || ext>size || size-ext<FW_OFFSET ||
        size-ext-FW_OFFSET<FW_HEADER) return PJ_CATALOG_INVALID_IMAGE;
    fw=image+ext+FW_OFFSET;
    remaining=size-ext-FW_OFFSET;
    header=rd32(fw+4u);
    if (rd32(fw)!=0x314d4124u || header<FW_HEADER || header>remaining)
        return PJ_CATALOG_INVALID_IMAGE;
    v->count=rd32(fw+36u);
    if (!v->count || v->count>MAX_BASE_MODULES ||
        (PJ_SIZE)v->count>(remaining-header)/MODULE_BYTES)
        return PJ_CATALOG_INVALID_IMAGE;
    table=(PJ_SIZE)v->count*MODULE_BYTES;
    v->modules=fw+header;
    v->configs=v->modules+table;
    v->config_capacity=(remaining-header-table)/CONFIG_BYTES;
    for (i=0u;i<v->count;++i) {
        const PJ_BYTE *m=v->modules+(PJ_SIZE)i*MODULE_BYTES;
        PJ_SIZE offset=rd16(m+68u), count=rd16(m+70u);
        if (rd32(m)!=0x454d4124u ||
            (count && (offset>v->config_capacity ||
                        count>v->config_capacity-offset)))
            return PJ_CATALOG_INVALID_IMAGE;
    }
    return PJ_CATALOG_OK;
}
static pj_catalog_status locate(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE *uuid, catalog_view *v, PJ_DWORD *index)
{
    PJ_DWORD i, found=0xffffffffu;
    pj_catalog_status s;
    if (!image || !uuid || !uuid_nonzero(uuid)) return PJ_CATALOG_INVALID_ARGUMENT;
    s=parse(image,size,v);
    if (s!=PJ_CATALOG_OK) return s;
    for (i=0u;i<v->count;++i) {
        const PJ_BYTE *m=v->modules+(PJ_SIZE)i*MODULE_BYTES;
        if (uuid_equal(m+12u,uuid)) {
            if (found!=0xffffffffu) return PJ_CATALOG_AMBIGUOUS;
            found=i;
        }
    }
    if (found==0xffffffffu) return PJ_CATALOG_NOT_FOUND;
    *index=found;
    return PJ_CATALOG_OK;
}
pj_catalog_status pj_firmware_find_module(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], pj_firmware_module *out)
{
    catalog_view v;
    PJ_DWORD i,j;
    const PJ_BYTE *m;
    pj_firmware_module result;
    pj_catalog_status s;
    if (!out) return PJ_CATALOG_INVALID_ARGUMENT;
    s=locate(image,size,uuid,&v,&i);
    if (s!=PJ_CATALOG_OK) return s;
    m=v.modules+(PJ_SIZE)i*MODULE_BYTES;
    result.module_id=i;
    result.affinity_mask=rd32(m+72u);
    result.type=rd32(m+28u);
    result.instance_max_count=rd16(m+76u);
    result.config_count=rd16(m+70u);
    for (j=0u;j<16u;++j) result.uuid[j]=m[12u+j];
    *out=result;
    return PJ_CATALOG_OK;
}
pj_catalog_status pj_firmware_find_config(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], PJ_DWORD ibs, PJ_DWORD obs, pj_firmware_config *out)
{
    catalog_view v;
    PJ_DWORD i,j, offset,count;
    const PJ_BYTE *m;
    pj_firmware_config result={0};
    int found=0;
    pj_catalog_status s;
    if (!out || !ibs || !obs) return PJ_CATALOG_INVALID_ARGUMENT;
    s=locate(image,size,uuid,&v,&i);
    if (s!=PJ_CATALOG_OK) return s;
    m=v.modules+(PJ_SIZE)i*MODULE_BYTES;
    offset=rd16(m+68u);
    count=rd16(m+70u);
    for (j=0u;j<count;++j) {
        const PJ_BYTE *c=v.configs+((PJ_SIZE)offset+j)*CONFIG_BYTES;
        PJ_DWORD instance_bytes=rd32(c+16u), cpc=rd32(c+36u);
        if (rd32(c+24u)!=ibs || rd32(c+28u)!=obs) continue;
        if (found && (result.instance_bytes!=instance_bytes || result.cpc!=cpc))
            return PJ_CATALOG_AMBIGUOUS;
        result.instance_bytes=instance_bytes;
        result.cpc=cpc;
        result.ibs=ibs;
        result.obs=obs;
        found=1;
    }
    if (!found) return PJ_CATALOG_NOT_FOUND;
    *out=result;
    return PJ_CATALOG_OK;
}

pj_catalog_status pj_firmware_load_profile(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], pj_firmware_profile *out)
{
    catalog_view v;
    PJ_DWORD i,j,offset,count;
    const PJ_BYTE *m;
    pj_firmware_profile result={0};
    pj_catalog_status s;
    if (!out) return PJ_CATALOG_INVALID_ARGUMENT;
    s=locate(image,size,uuid,&v,&i);
    if (s!=PJ_CATALOG_OK) return s;
    m=v.modules+(PJ_SIZE)i*MODULE_BYTES;
    offset=rd16(m+68u);
    count=rd16(m+70u);
    if (!count) return PJ_CATALOG_NOT_FOUND;
    if (count>PJ_CATALOG_MAX_CONFIGS) return PJ_CATALOG_INVALID_IMAGE;
    result.module.module_id=i;
    result.module.affinity_mask=rd32(m+72u);
    result.module.type=rd32(m+28u);
    result.module.instance_max_count=rd16(m+76u);
    result.module.config_count=(PJ_WORD)count;
    for (j=0u;j<16u;++j) result.module.uuid[j]=m[12u+j];
    result.count=count;
    for (j=0u;j<count;++j) {
        const PJ_BYTE *c=v.configs+((PJ_SIZE)offset+j)*CONFIG_BYTES;
        pj_firmware_config *record=&result.configs[j];
        record->instance_bytes=rd32(c+16u);
        record->ibs=rd32(c+24u);
        record->obs=rd32(c+28u);
        record->cpc=rd32(c+36u);
    }
    *out=result;
    return PJ_CATALOG_OK;
}

pj_catalog_status pj_firmware_profile_config(const pj_firmware_profile *profile,
    PJ_DWORD ibs, PJ_DWORD obs, pj_firmware_config *out)
{
    PJ_DWORD i;
    pj_firmware_config result={0};
    int found=0;
    if (!profile || !out || !ibs || !obs) return PJ_CATALOG_INVALID_ARGUMENT;
    if (!profile->count) return PJ_CATALOG_NOT_FOUND;
    if (profile->count>PJ_CATALOG_MAX_CONFIGS ||
        profile->count!=profile->module.config_count)
        return PJ_CATALOG_INVALID_IMAGE;
    for (i=0u;i<profile->count;++i) {
        const pj_firmware_config *c=&profile->configs[i];
        if (c->ibs!=ibs || c->obs!=obs) continue;
        if (found && (result.instance_bytes!=c->instance_bytes || result.cpc!=c->cpc))
            return PJ_CATALOG_AMBIGUOUS;
        result=*c;
        found=1;
    }
    if (!found) return PJ_CATALOG_NOT_FOUND;
    *out=result;
    return PJ_CATALOG_OK;
}

pj_catalog_status pj_firmware_plan_ll(const pj_firmware_profile *profile,
    PJ_DWORD ibs, PJ_DWORD obs, pj_firmware_config *out, PJ_DWORD *pipeline_pages)
{
    /* Low-latency task and queue accounting from core/ipc4-topology.h
     * (Intel copyright, dual BSD/GPL). Work in bytes before rounding pages. */
    const PJ_DWORD pipeline_object = 448u, module_item = 12u;
    const PJ_DWORD ll_task_aligned = 128u, ll_list_item = 12u;
    const PJ_DWORD queue_count = 8u, queue_object = 128u;
    const PJ_DWORD page_size = 4096u;
    const PJ_DWORD limit = 20u * page_size;
    pj_firmware_config result = {0};
    PJ_DWORD i, common_size, task_bytes, queue_bytes;
    int same_size = 1, exact = 0;

    if (!profile || !out || !pipeline_pages || !ibs || !obs)
        return PJ_CATALOG_INVALID_ARGUMENT;
    if (!profile->count || !profile->module.instance_max_count ||
        !(profile->module.affinity_mask & 1u) || (profile->module.type & 0x60u) != 0x20u)
        return PJ_CATALOG_NOT_FOUND;
    if (profile->count > PJ_CATALOG_MAX_CONFIGS ||
        profile->count != profile->module.config_count || ibs > limit || obs > limit)
        return PJ_CATALOG_INVALID_IMAGE;

    common_size = profile->configs[0].instance_bytes;
    for (i = 0; i < profile->count; ++i) {
        const pj_firmware_config *record = &profile->configs[i];
        if (record->instance_bytes != common_size) same_size = 0;
        if (record->ibs != ibs || record->obs != obs) continue;
        if (exact && result.instance_bytes != record->instance_bytes)
            return PJ_CATALOG_AMBIGUOUS;
        exact = 1;
        result.instance_bytes = record->instance_bytes;
        if (result.cpc < record->cpc) result.cpc = record->cpc;
    }
    if (!exact) {
        if (!same_size) return PJ_CATALOG_NOT_FOUND;
        result.instance_bytes = common_size;
    }
    if (result.instance_bytes > limit) return PJ_CATALOG_INVALID_IMAGE;
    task_bytes = pipeline_object + module_item + result.instance_bytes +
                 ll_task_aligned + queue_count * module_item + ll_list_item;
    queue_bytes = queue_count * (queue_object + ((ibs + 63u) & ~63u));
    if (task_bytes + queue_bytes > limit) return PJ_CATALOG_INVALID_IMAGE;
    result.ibs = ibs;
    result.obs = obs;
    *pipeline_pages = (task_bytes + queue_bytes + page_size - 1u) / page_size;
    *out = result;
    return PJ_CATALOG_OK;
}
