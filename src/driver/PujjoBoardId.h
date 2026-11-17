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
#ifdef __cplusplus
}
#endif
#endif
