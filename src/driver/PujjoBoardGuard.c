/* SPDX-License-Identifier: BSD-3-Clause */
#include <ntddk.h>
#include <aux_klib.h>
#include "PujjoBoardGuard.h"
#include "PujjoBoardId.h"

#define PUJJO_RSMB_PROVIDER 'RSMB'
#define PUJJO_MAX_SMBIOS_BYTES (256u * 1024u)

BOOLEAN PujjoBoardIdentityMatches(void)
{
    NTSTATUS st;
    ULONG size = 0, actual = 0;
    PJ_BYTE *raw = NULL;
    BOOLEAN accepted = FALSE;

    PAGED_CODE(); /* DevicePrepareHardware executes at PASSIVE_LEVEL. */
    st = AuxKlibInitialize();
    if (!NT_SUCCESS(st)) return FALSE;

    st = AuxKlibGetSystemFirmwareTable(PUJJO_RSMB_PROVIDER, 0, NULL, 0, &size);
    if (st != STATUS_BUFFER_TOO_SMALL || size < 8 || size > PUJJO_MAX_SMBIOS_BYTES)
        return FALSE;

    raw = (PJ_BYTE *)ExAllocatePool2(POOL_FLAG_PAGED, size, 'jPuP');
    if (!raw) return FALSE;
    st = AuxKlibGetSystemFirmwareTable(PUJJO_RSMB_PROVIDER, 0, raw, size, &actual);
    if (NT_SUCCESS(st) && actual >= 8 && actual <= size) {
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
        accepted = PujjoMatchRawSmbiosRev3(raw, actual) ? TRUE : FALSE;
#else
        accepted = PujjoMatchRawSmbios(raw, actual) ? TRUE : FALSE;
#endif
    }
    ExFreePoolWithTag(raw, 'jPuP');
    return accepted;
}
