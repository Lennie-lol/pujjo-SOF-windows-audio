/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoDmicProfile.h"

static PJ_DWORD word(const PJ_BYTE *p)
{
    return (PJ_DWORD)p[0] | ((PJ_DWORD)p[1] << 8) |
           ((PJ_DWORD)p[2] << 16) | ((PJ_DWORD)p[3] << 24);
}

int pj_dmic_validate(const PJ_BYTE *data, PJ_SIZE bytes, pj_dmic_profile *out)
{
    /* ALSA v1.2.13 encoder, SOF v2.12 ADL DMIC reference. The timestamp,
     * FIFO and controller routing words must match the reviewed geometry. */
    static const PJ_DWORD header[10] = {
        1u, 0xffff1010u, 0xffffffffu, 0xffffffffu, 0xffffffffu,
        16u, 3u, 0x3a0003u, 0u, 3u
    };
    pj_dmic_profile result = {0};
    PJ_SIZE pos = 40u;
    if (!out) return 0;
    *out = result;
    if (!data || bytes < 232u || bytes > PJ_DMIC_MAX_PAYLOAD || (bytes & 3u))
        return 0;
    for (PJ_DWORD i = 0; i < 10u; ++i)
        if (word(data + 4u*i) != header[i]) return 0;

    for (PJ_DWORD c = 0; c < 2u; ++c) {
        PJ_DWORD cfg, mic, divider, decim;
        if (bytes-pos < 96u) return 0;
        cfg = word(data + pos + 4u);
        mic = word(data + pos + 12u);
        if (word(data + pos) != 0xc001u ||
            (mic & ~0xff00u) != 3u || (cfg & ~0x0f00ff00u) ||
            word(data + pos + 8u) || word(data + pos + 16u) ||
            word(data + pos + 20u) || word(data + pos + 24u) ||
            word(data + pos + 28u))
            return 0;
        divider = ((mic >> 8) & 255u) + 2u;
        decim = ((cfg >> 8) & 255u) + 1u;
        /* Division avoids overflow even for adversarial divider fields. */
        if (decim < 5u || decim > 31u || ((cfg >> 24) & 15u) > 12u ||
            divider < 8u || divider > 16u ||
            5u*(divider/2u) < 2u*divider || 5u*((divider+1u)/2u) > 3u*divider ||
            (c && divider != result.clock_divider))
            return 0;
        result.clock_divider = divider;
        result.pdm_clock_hz = PJ_DMIC_IO_CLOCK / divider;
        result.cic_decimation[c] = decim;
        for (PJ_DWORD f = 0; f < 2u; ++f) {
            const PJ_BYTE *fir = data + pos + 32u + 32u*f;
            PJ_DWORD fc = word(fir + 4u);
            PJ_DWORD n = (fc & 255u) + 1u;
            PJ_DWORD ratio = ((fc >> 16) & 31u) + 1u;
            if (word(fir) != 0x91u || (fc & ~0x001f0fffu) ||
                n > 250u || ratio < 2u || ratio > 20u ||
                ((fc >> 8) & 15u) > 8u ||
                PJ_DMIC_RATE*divider*decim*ratio != PJ_DMIC_IO_CLOCK)
                return 0;
            for (PJ_DWORD i = 2u; i < 8u; ++i)
                if (word(fir + 4u*i)) return 0;
            result.taps[c][f] = n;
            result.fir_decimation[c][f] = ratio;
        }
        pos += 96u;
        for (PJ_DWORD f = 0; f < 2u; ++f) {
            PJ_DWORD n = result.taps[c][f], nonzero = 0u;
            if (bytes-pos < 4u*n) return 0;
            for (PJ_DWORD i = 0; i < n; ++i) {
                PJ_DWORD coefficient = word(data + pos + 4u*i);
                if (coefficient > 0xfffffu) return 0;
                nonzero |= coefficient;
            }
            if (!nonzero) return 0;
            pos += 4u*n;
        }
    }
    if (pos != bytes) return 0;
    result.bytes = (PJ_DWORD)bytes;
    *out = result;
    return 1;
}
