/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoDmaPlan.h"

static int add_offset(PJ_DWORD low, PJ_DWORD high, PJ_DWORD offset,
                      PJ_DWORD *new_low, PJ_DWORD *new_high)
{
    PJ_DWORD sum = low + offset;
    PJ_DWORD carry = sum < low ? 1u : 0u;
    if (carry && high == 0xffffffffu) return 0;
    *new_low = sum;
    *new_high = high + carry;
    return 1;
}
int pj_hda_dma_span_valid(PJ_DWORD low, PJ_DWORD high, PJ_DWORD bytes, int dma64)
{
    PJ_DWORD last_low, last_high;
    return bytes && !(low & 127u) &&
        add_offset(low, high, bytes - 1u, &last_low, &last_high) &&
        (dma64 || last_high == 0);
}
int pj_dma_plan_pair(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                     PJ_DWORD split, pj_dma_plan *out)
{
    pj_dma_plan plan;
    PJ_DWORD last_low, last_high;
    if (!out || total > PJ_DMA_MAX_BYTES || total < 256u ||
        split == 0u || split >= total ||
        (low & 127u) != 0u || (split & 127u) != 0u)
        return 0;
    if (!add_offset(low, high, total - 1u, &last_low, &last_high) ||
        !add_offset(low, high, split, &plan.second.low, &plan.second.high))
        return 0;
    (void)last_low; (void)last_high;
    plan.first.low = low;
    plan.first.high = high;
    plan.first.length = split;
    plan.second.length = total - split;
    *out = plan;
    return 1;
}

int pj_dma_period_count(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                        PJ_DWORD period, PJ_DWORD *count)
{
    PJ_DWORD periods, last_low, last_high;
    if (!count || total > PJ_DMA_MAX_BYTES || (low & 127u) ||
        period < 128u || (period & 127u) || total % period)
        return 0;
    periods = total / period;
    if (periods < 2u || periods > PJ_DMA_MAX_PERIODS ||
        !add_offset(low, high, total - 1u, &last_low, &last_high))
        return 0;
    *count = periods;
    return 1;
}

int pj_dma_period_segment(PJ_DWORD low, PJ_DWORD high, PJ_DWORD total,
                          PJ_DWORD period, PJ_DWORD index, pj_dma_segment *out)
{
    PJ_DWORD count;
    pj_dma_segment segment;
    if (!out || !pj_dma_period_count(low, high, total, period, &count) ||
        index >= count ||
        !add_offset(low, high, index * period, &segment.low, &segment.high))
        return 0;
    segment.length = period;
    *out = segment;
    return 1;
}
int pj_pcm_period(PJ_DWORD rate, PJ_DWORD channels, PJ_DWORD bits,
                  PJ_DWORD valid_bits, PJ_DWORD period_ms,
                  PJ_DWORD ring_bytes, PJ_DWORD *out_bytes)
{
    PJ_DWORD frames_ms, frame_bytes, period_bytes;
    if (!out_bytes || rate < 8000u || rate > 192000u ||
        channels == 0u || channels > 8u ||
        (bits != 16u && bits != 24u && bits != 32u) ||
        valid_bits == 0u || valid_bits > bits ||
        period_ms == 0u || period_ms > 1000u ||
        ring_bytes < 256u || ring_bytes > PJ_DMA_MAX_BYTES ||
        (ring_bytes & 255u) != 0u)
        return 0;
    /* The caps above bound all multiplications. */
    frames_ms = rate * period_ms;
    if (frames_ms % 1000u != 0u) return 0;
    frame_bytes = channels * (bits / 8u);
    period_bytes = (frames_ms / 1000u) * frame_bytes;
    if (!period_bytes || ring_bytes / 2u < period_bytes ||
        (ring_bytes / 2u) % frame_bytes != 0u)
        return 0;
    *out_bytes = period_bytes;
    return 1;
}

int pj_hda_stream_slot(PJ_DWORD captures, PJ_DWORD playbacks, PJ_DWORD capture,
                       PJ_DWORD slot, PJ_DWORD bar_bytes, PJ_DWORD *index)
{
    PJ_DWORD resolved, offset;
    if (!index || captures>15u || playbacks>15u || capture>1u ||
        slot >= (capture ? captures : playbacks)) return 0;
    resolved = capture ? slot : captures + slot;
    /* Intel HDA SD0 starts at 0x80; each descriptor occupies 0x20 bytes. */
    offset = 0x80u + resolved * 0x20u;
    if (offset>bar_bytes || bar_bytes-offset<0x20u) return 0;
    *index=resolved;
    return 1;
}

int pj_pcm_notification_ring(PJ_DWORD rate, PJ_DWORD channels, PJ_DWORD bits,
    PJ_DWORD valid_bits, PJ_DWORD requested, PJ_DWORD *ring_bytes, PJ_DWORD *period_ms)
{
    PJ_DWORD frame, ms;
    if (!ring_bytes || !period_ms || !requested || requested > PJ_DMA_MAX_BYTES ||
        rate < 8000u || rate > 192000u || channels < 1u || channels > 8u ||
        (bits != 16u && bits != 32u) || !valid_bits || valid_bits > bits)
        return 0;
    frame = channels * (bits / 8u);
    for (ms = 1u; ms <= 1000u; ++ms) {
        PJ_DWORD frames_ms = rate * ms;
        PJ_DWORD period = frames_ms / 1000u * frame;
        if (frames_ms % 1000u || (period & 127u) || period * 2u < requested)
            continue;
        *ring_bytes = period * 2u;
        *period_ms = ms;
        return 1;
    }
    return 0;
}

int pj_dmic_voice_ring(PJ_DWORD requested_pcm_bytes, PJ_DWORD *dma_bytes, PJ_DWORD *period_ms)
{
    if (!requested_pcm_bytes || requested_pcm_bytes > 9600u) return 0;
    if (requested_pcm_bytes < 3840u) requested_pcm_bytes = 3840u;
    return pj_pcm_notification_ring(48000u, 4u, 32u, 32u,
        requested_pcm_bytes * 8u, dma_bytes, period_ms);
}
