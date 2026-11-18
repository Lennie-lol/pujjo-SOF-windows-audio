/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_FIRMWARE_CATALOG_H
#define PUJJO_FIRMWARE_CATALOG_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Base-firmware IPC4 metadata only: no trust, compatibility, or boot claims.
 * UUID is the 16 raw bytes in the signed payload's module entry.
 * Output remains unchanged on failure. No external libraries supported. */
typedef enum {
    PJ_CATALOG_OK = 0, PJ_CATALOG_INVALID_ARGUMENT,
    PJ_CATALOG_INVALID_IMAGE, PJ_CATALOG_NOT_FOUND, PJ_CATALOG_AMBIGUOUS
} pj_catalog_status;
typedef struct {
    PJ_DWORD module_id; /* base firmware table index, not the $AME marker */
    PJ_DWORD affinity_mask;
    PJ_WORD instance_max_count;
    PJ_WORD config_count;
    PJ_BYTE uuid[16];
} pj_firmware_module;
typedef struct {
    PJ_DWORD instance_bytes, cpc, ibs, obs;
} pj_firmware_config;
pj_catalog_status pj_firmware_find_module(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], pj_firmware_module *out);
/* Exact IBS/OBS lookup from the matching module's config records.
 * Multiple identical records are permitted only when resulting CPC/BSS agree.
 * Does not establish a valid DSP core, format, or firmware budget. */
pj_catalog_status pj_firmware_find_config(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], PJ_DWORD ibs, PJ_DWORD obs, pj_firmware_config *out);
/* Bounded, immutable copy of manifest configuration records. No pointers to
 * a released firmware image. 64 is a software capacity, not a hardware limit. */
#define PJ_CATALOG_MAX_CONFIGS 64u
typedef struct {
    pj_firmware_module module;
    PJ_DWORD count;
    pj_firmware_config configs[PJ_CATALOG_MAX_CONFIGS];
} pj_firmware_profile;
pj_catalog_status pj_firmware_load_profile(const PJ_BYTE *image, PJ_SIZE size,
    const PJ_BYTE uuid[16], pj_firmware_profile *out);
pj_catalog_status pj_firmware_profile_config(const pj_firmware_profile *profile,
    PJ_DWORD ibs, PJ_DWORD obs, pj_firmware_config *out);
#ifdef __cplusplus
}
#endif
#endif
