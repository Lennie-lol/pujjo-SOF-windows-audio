/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PUJJO_BOARD_GUARD_H
#define PUJJO_BOARD_GUARD_H
#include <ntddk.h>
/* Returns TRUE only if SMBIOS baseboard reports Google Pujjo or Pujjo1e. */
BOOLEAN PujjoBoardIdentityMatches(void);
#endif
