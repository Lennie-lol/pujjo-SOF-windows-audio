/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_BOARD_ID_H
#define PUJJO_BOARD_ID_H
#include "PujjoFixedTypes.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure, bounded SMBIOS parser: no OS functions and no hardware writes. */
int PujjoIsAllowedProduct(const char *product);
int PujjoMatchRawSmbios(const PJ_BYTE *buffer, PJ_SIZE length);
/* Decode a type-0 PCI memory BAR without probing size or writing config
 * space. A 64-bit BAR consumes two slots. Outputs change only on success. */
int PujjoReadMemoryBar(const PJ_DWORD bars[6], PJ_DWORD index,
                       PJ_DWORD *low, PJ_DWORD *high);
#ifdef __cplusplus
}
#endif
#endif
