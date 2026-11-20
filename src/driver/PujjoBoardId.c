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

int PujjoReadMemoryBar(const PJ_DWORD bars[6], PJ_DWORD index,
                       PJ_DWORD *low, PJ_DWORD *high)
{
    PJ_DWORD i;
    if (!bars || !low || !high || index >= 6u) return 0;
    for (i = 0; i <= index;) {
        PJ_DWORD raw = bars[i];
        PJ_DWORD type = raw & 7u;
        PJ_DWORD width = type == 4u ? 2u : 1u;
        if (i + width > 6u) return 0;
        if (i == index) {
            PJ_DWORD lo = raw & ~15u;
            PJ_DWORD hi = width == 2u ? bars[i + 1u] : 0u;
            if (!(type == 0u || type == 4u) || raw == 0xffffffffu ||
                (!lo && !hi) || hi == 0xffffffffu)
                return 0;
            *low = lo;
            *high = hi;
            return 1;
        }
        i += width;
    }
    return 0;
}

/* The diagnostic is scoped to the one recovery-evidenced baseboard revision.
 * Require a complete unambiguous table rather than a matching first record.
 * SMBIOS Type 2 version (revision) string index is byte 6. */
int PujjoMatchRawSmbiosRev3(const PJ_BYTE *raw, PJ_SIZE raw_size)
{
    PJ_SIZE size, pos, end;
    unsigned boards = 0;
    int match = 0;
    if (!raw || raw_size < 8) return 0;
    size = (PJ_SIZE)raw[4] | ((PJ_SIZE)raw[5] << 8) |
           ((PJ_SIZE)raw[6] << 16) | ((PJ_SIZE)raw[7] << 24);
    if (!size || size != raw_size - 8) return 0;
    end = raw_size;
    for (pos = 8; pos < end;) {
        const PJ_BYTE *hdr = raw + pos;
        PJ_SIZE len, start, next;
        if (end - pos < 4) return 0;
        len = hdr[1];
        if (len < 4 || len > end - pos) return 0;
        start = pos + len;
        next = start;
        while (next + 1 < end && !(raw[next] == 0 && raw[next+1] == 0)) ++next;
        if (next + 1 >= end) return 0;
        if (hdr[0] == 2) {
            if (++boards != 1 || len < 8) return 0;
            match = smbios_string_equals(raw + start, next + 2 - start, hdr[4], "Google") &&
                    smbios_string_equals(raw + start, next + 2 - start, hdr[5], "Pujjo") &&
                    smbios_string_equals(raw + start, next + 2 - start, hdr[6], "rev3");
        }
        if (hdr[0] == 127)
            return len == 4 && next == start && next + 2 == end && boards == 1 && match;
        pos = next + 2;
    }
    return 0;
}
