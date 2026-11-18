/* SPDX-License-Identifier: BSD-3-Clause */
/* Pure DMA geometry. Does not allocate memory, program hardware or establish ownership. */
#ifndef PUJJO_DMA_PLAN_H
#define PUJJO_DMA_PLAN_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PJ_DMA_MAX_BYTES (16u * 1024u * 1024u)
typedef struct { PJ_DWORD low, high, length; } pj_dma_segment;
typedef struct { pj_dma_segment first, second; } pj_dma_plan;
/* Output unchanged on failure. Both segment starts must be 128-byte aligned. */
int pj_dma_plan_pair(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                     PJ_DWORD split, pj_dma_plan *out);
/* Conservative arithmetic policy, not verified hardware-format capabilities. */
int pj_pcm_period(PJ_DWORD rate, PJ_DWORD channels, PJ_DWORD bits,
                  PJ_DWORD valid_bits, PJ_DWORD period_ms,
                  PJ_DWORD ring_bytes, PJ_DWORD *out_bytes);
/* Resolve a direction-relative HDA descriptor only within reported counts
 * and the mapped BAR. Output unchanged on failure. No resource reservation
 * or hardware activation is implied by a successful geometry check. */
int pj_hda_stream_slot(PJ_DWORD captures, PJ_DWORD playbacks, PJ_DWORD capture,
                       PJ_DWORD slot, PJ_DWORD bar_bytes, PJ_DWORD *index);
#ifdef __cplusplus
}
#endif
#endif
