/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_BOARD_ID_H
#define PUJJO_BOARD_ID_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Pure, bounded SMBIOS parser: no OS functions and no hardware writes. */
int PujjoIsAllowedProduct(const char *product);
int PujjoMatchRawSmbios(const uint8_t *buffer, size_t length);
#ifdef __cplusplus
}
#endif
#endif
