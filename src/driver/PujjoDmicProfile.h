/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_DMIC_PROFILE_H
#define PUJJO_DMIC_PROFILE_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Only the independently encoded Pujjo DMIC0 reference geometry is in scope.
 * Validation is structural, not authorization to program the DSP or a claim
 * that the firmware accepts the coefficients. */
#define PJ_DMIC_RATE 48000u
#define PJ_DMIC_CHANNELS 4u
#define PJ_DMIC_BITS 32u
#define PJ_DMIC_CHUNK_BYTES 768u
#define PJ_DMIC_IO_CLOCK 38400000u
#define PJ_DMIC_MAX_PAYLOAD 8192u

typedef struct {
    PJ_DWORD bytes;
    PJ_DWORD clock_divider;
    PJ_DWORD pdm_clock_hz;
    PJ_DWORD cic_decimation[2];
    PJ_DWORD fir_decimation[2][2];
    PJ_DWORD taps[2][2];
} pj_dmic_profile;

/* Output is cleared on every failure; accepts unaligned serialized input.
 * No allocation, register access, floating point, or platform dependency. */
int pj_dmic_validate(const PJ_BYTE *data, PJ_SIZE bytes, pj_dmic_profile *out);
#ifdef __cplusplus
}
#endif
#endif
