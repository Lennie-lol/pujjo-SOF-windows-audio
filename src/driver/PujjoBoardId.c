/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoBoardId.h"

static int ascii_equal_n(const PJ_BYTE *a, PJ_SIZE length, const char *b)
{
    PJ_SIZE i;
    for (i = 0; i < length; ++i) {
        unsigned x = a[i];
        unsigned y = (unsigned char)b[i];
        if (!y) return 0;
        if (x >= 'A' && x <= 'Z') x += ('a' - 'A');
        if (y >= 'A' && y <= 'Z') y += ('a' - 'A');
        if (x != y) return 0;
    }
    return b[length] == 0;
}

int PujjoIsAllowedProduct(const char *product)
{
    const PJ_BYTE *p = (const PJ_BYTE *)product;
    PJ_SIZE n = 0;
    if (!p) return 0;
    /* Reject nonterminated/unexpectedly long input before comparisons. */
    while (n < 32 && p[n]) ++n;
    if (n == 32) return 0;
    return ascii_equal_n(p, n, "pujjo") || ascii_equal_n(p, n, "pujjo1e");
}

static int smbios_string_equals(const PJ_BYTE *strings, PJ_SIZE bytes,
                                unsigned index, const char *expected)
{
    PJ_SIZE offset = 0;
    unsigned current = 1;
    if (index == 0) return 0;
    while (offset < bytes && strings[offset] != 0) {
        PJ_SIZE end = offset;
        while (end < bytes && strings[end]) ++end;
        if (end == bytes) return 0;
        if (current == index)
            return ascii_equal_n(strings + offset, end - offset, expected);
        offset = end + 1;
        ++current;
    }
    return 0;
}

/* Returns 1 only for SMBIOS baseboard manufacturer Google, product Pujjo/Pujjo1e.
 * RawSMBIOSData header: 4 version bytes + little-endian 32-bit table length.
 * SMBIOS type 2 fields: manufacturer index at 4, product index at 5. */
int PujjoMatchRawSmbios(const PJ_BYTE *raw, PJ_SIZE raw_size)
{
    PJ_SIZE table_size, pos;
    if (!raw || raw_size < 8) return 0;
    table_size = (PJ_SIZE)raw[4] | ((PJ_SIZE)raw[5] << 8) |
                 ((PJ_SIZE)raw[6] << 16) | ((PJ_SIZE)raw[7] << 24);
    if (!table_size || table_size > raw_size - 8) return 0;
    pos = 8;
    while (pos < 8 + table_size) {
        const PJ_BYTE *hdr = raw + pos;
        PJ_SIZE length, string_start, next, table_end = 8 + table_size;
        if (table_end - pos < 4) return 0;
        length = hdr[1];
        if (length < 4 || length > table_end - pos) return 0;
        string_start = pos + length;
        next = string_start;
        /* Look for the mandatory double NUL after the string area. */
        while (next + 1 < table_end && !(raw[next] == 0 && raw[next+1] == 0)) ++next;
        if (next + 1 >= table_end) return 0;
        if (hdr[0] == 2 && length >= 8) {
            const PJ_BYTE *strings = raw + string_start;
            PJ_SIZE bytes = next + 2 - string_start;
            int google = smbios_string_equals(strings, bytes, hdr[4], "Google");
            int board = smbios_string_equals(strings, bytes, hdr[5], "Pujjo") ||
                        smbios_string_equals(strings, bytes, hdr[5], "Pujjo1e");
            if (google && board) return 1;
        }
        if (hdr[0] == 127) return 0;
        pos = next + 2;
    }
    return 0;
}
