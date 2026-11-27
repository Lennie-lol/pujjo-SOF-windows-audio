/* SPDX-License-Identifier: BSD-3-Clause */
/* Pure DMA geometry. Does not allocate memory, program hardware or establish ownership. */
#ifndef PUJJO_DMA_PLAN_H
#define PUJJO_DMA_PLAN_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PJ_DMA_MAX_BYTES (16u * 1024u * 1024u)
#define PJ_DMA_MAX_PERIODS 256u
typedef struct { PJ_DWORD low, high, length; } pj_dma_segment;
typedef struct { pj_dma_segment first, second; } pj_dma_plan;
int pj_hda_dma_span_valid(PJ_DWORD low, PJ_DWORD high, PJ_DWORD bytes, int dma64);
/* Output unchanged on failure. Both segment starts must be 128-byte aligned. */
int pj_dma_plan_pair(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                     PJ_DWORD split, pj_dma_plan *out);
/* A cyclic HDA BDL entry for each complete period. Query the number of
 * periods before writing entries. Outputs remain unchanged on failure. */
int pj_dma_period_count(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                        PJ_DWORD period, PJ_DWORD *count);
int pj_dma_period_segment(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                          PJ_DWORD period, PJ_DWORD index, pj_dma_segment *out);
/* Conservative arithmetic policy, not verified hardware-format capabilities. */
int pj_pcm_period(PJ_DWORD rate, PJ_DWORD channels, PJ_DWORD bits,
                  PJ_DWORD valid_bits, PJ_DWORD period_ms,
                  PJ_DWORD ring_bytes, PJ_DWORD *out_bytes);
/* Round a WaveRT two-period request up to complete, DMA-aligned periods. */
int pj_pcm_notification_ring(PJ_DWORD rate, PJ_DWORD channels, PJ_DWORD bits,
    PJ_DWORD valid_bits, PJ_DWORD requested, PJ_DWORD *ring_bytes, PJ_DWORD *period_ms);
/* Two periods of four S32 slots at 48 kHz back each mono S16 client ring.
 * Bound conversion work per DPC and allow for the Windows periodic timer. */
int pj_dmic_voice_ring(PJ_DWORD requested_pcm_bytes, PJ_DWORD *dma_bytes, PJ_DWORD *period_ms);
/* Resolve a direction-relative HDA descriptor only within reported counts
 * and the mapped BAR. Output unchanged on failure. No resource reservation
 * or hardware activation is implied by a successful geometry check. */
int pj_hda_stream_slot(PJ_DWORD captures, PJ_DWORD playbacks, PJ_DWORD capture,
                       PJ_DWORD slot, PJ_DWORD bar_bytes, PJ_DWORD *index);
#ifdef __cplusplus
}
#endif
#endif
