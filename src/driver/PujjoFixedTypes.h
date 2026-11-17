/* SPDX-License-Identifier: BSD-3-Clause */
/* Shared fixed-width types for portable Pujjo validation code.
 * Kernel mode MUST NOT import MSVC's desktop CRT stdint.h/stddef.h: doing so
 * mixes the vcruntime headers with the WDK kernel CRT. */
#ifndef PUJJO_FIXED_TYPES_H
#define PUJJO_FIXED_TYPES_H
#if defined(_KERNEL_MODE)
#include <ntddk.h>
typedef UCHAR PJ_BYTE;
typedef USHORT PJ_WORD;
typedef ULONG PJ_DWORD;   /* 32-bit on Windows, including x64 */
typedef SIZE_T PJ_SIZE;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t PJ_BYTE;
typedef uint16_t PJ_WORD;
typedef uint32_t PJ_DWORD;
typedef size_t PJ_SIZE;
#endif
#endif /* PUJJO_FIXED_TYPES_H */
