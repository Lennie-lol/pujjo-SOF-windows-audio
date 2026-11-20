/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/PujjoBoardId.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static size_t make_smbios(uint8_t *out, size_t capacity,
                          const char *vendor, const char *product,
                          int with_system_type)
{
    uint8_t *p = out + 8;
    size_t vlen = strlen(vendor) + 1, plen = strlen(product) + 1;
    assert(capacity >= 8 + 8 + vlen + plen + 2 + 6 + 16);
    memset(out, 0, capacity);
    if (with_system_type) {
        const uint8_t type1[] = { 1, 8, 1, 0, 0, 1, 0, 0 };
        memcpy(p, type1, sizeof(type1)); p += sizeof(type1);
        memcpy(p, "Example", 8); p += 8;
        *p++ = 0;
    }
    {
        const uint8_t type2[] = { 2, 8, 2, 0, 1, 2, 0, 0 };
        memcpy(p, type2, sizeof(type2)); p += sizeof(type2);
    }
    memcpy(p, vendor, vlen); p += vlen;
    memcpy(p, product, plen); p += plen;
    *p++ = 0; /* final double NUL after last string */
    {
        const uint8_t end[] = { 127, 4, 3, 0, 0, 0 };
        memcpy(p, end, sizeof(end)); p += sizeof(end);
    }
    size_t bytes = (size_t)(p - out);
    uint32_t table_len = (uint32_t)(bytes - 8);
    out[0] = 0; out[1] = 3; out[2] = 4; out[3] = 0;
    out[4] = (uint8_t)(table_len & 0xff);
    out[5] = (uint8_t)((table_len >> 8) & 0xff);
    out[6] = (uint8_t)((table_len >> 16) & 0xff);
    out[7] = (uint8_t)((table_len >> 24) & 0xff);
    return bytes;
}

static size_t make_rev3(uint8_t *raw, const char *product, const char *revision)
{
    size_t n = make_smbios(raw, 512, "Google", product, 0);
    size_t bytes = strlen(revision) + 1;
    size_t term = n - 7;
    memmove(raw + term + bytes, raw + term, 7);
    memcpy(raw + term, revision, bytes);
    raw[8 + 6] = 3;
    n += bytes;
    raw[4] = (uint8_t)(n-8);
    return n;
}

int main(void)
{
    uint8_t raw[512];
    size_t len;
    int checks = 0;
#define EXPECT_TRUE(X) do { assert((X)); ++checks; } while (0)
#define EXPECT_FALSE(X) do { assert(!(X)); ++checks; } while (0)

    EXPECT_TRUE(PujjoIsAllowedProduct("Pujjo"));
    EXPECT_TRUE(PujjoIsAllowedProduct("pujjo1e"));
    EXPECT_FALSE(PujjoIsAllowedProduct("pujjoteen"));
    EXPECT_FALSE(PujjoIsAllowedProduct("PujjoFlex"));
    EXPECT_FALSE(PujjoIsAllowedProduct(NULL));
    len = make_smbios(raw, sizeof(raw), "Google", "Pujjo", 0);
    EXPECT_TRUE(PujjoMatchRawSmbios(raw, len));
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len - 1));
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, 4));
    len = make_smbios(raw, sizeof(raw), "Google", "Pujjo1e", 1);
    EXPECT_TRUE(PujjoMatchRawSmbios(raw, len));
    len = make_smbios(raw, sizeof(raw), "google", "PUJJO", 1);
    EXPECT_TRUE(PujjoMatchRawSmbios(raw, len));
    len = make_smbios(raw, sizeof(raw), "Lenovo", "Pujjo", 1);
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len));
    len = make_smbios(raw, sizeof(raw), "Google", "Pujjoteen", 1);
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len));
    len = make_smbios(raw, sizeof(raw), "Google", "PujjoX", 1);
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len));
    raw[4] = 0xff;
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len));
    len = make_smbios(raw, sizeof(raw), "Google", "Pujjo", 0);
    raw[9] = 255; /* length overruns SMBIOS table */
    EXPECT_FALSE(PujjoMatchRawSmbios(raw, len));
    len = make_rev3(raw, "Pujjo", "rev3");
    EXPECT_TRUE(PujjoMatchRawSmbiosRev3(raw,len));
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len-1));
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len+1));
    raw[14]=0;
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len));
    len = make_rev3(raw, "Pujjo", "rev2");
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len));
    len = make_rev3(raw, "Pujjo1e", "rev3");
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len));
    len = make_rev3(raw, "Pujjo", "rev3");
    raw[len-6]=3;
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len));
    len = make_rev3(raw, "Pujjo", "rev3");
    size_t ep = len-6, board_bytes = ep-8;
    memmove(raw+ep+board_bytes,raw+ep,6);
    memcpy(raw+ep,raw+8,board_bytes);
    len+=board_bytes;
    raw[4]=(uint8_t)(len-8);
    EXPECT_FALSE(PujjoMatchRawSmbiosRev3(raw,len));
    PJ_DWORD bars[6] = {0xdeeac004u, 0, 0, 0, 0xdef00004u, 1u};
    PJ_DWORD low = 0, high = 0;
    EXPECT_TRUE(PujjoReadMemoryBar(bars, 0, &low, &high));
    EXPECT_TRUE(low == 0xdeeac000u && high == 0u);
    EXPECT_FALSE(PujjoReadMemoryBar(bars, 1, &low, &high));
    EXPECT_TRUE(PujjoReadMemoryBar(bars, 4, &low, &high));
    EXPECT_TRUE(low == 0xdef00000u && high == 1u);
    EXPECT_FALSE(PujjoReadMemoryBar(bars, 5, &low, &high));
    bars[4] = 0xdef00001u;
    EXPECT_FALSE(PujjoReadMemoryBar(bars, 4, &low, &high));
    EXPECT_TRUE(low == 0xdef00000u && high == 1u);
    bars[4] = 0xba000008u;
    EXPECT_TRUE(PujjoReadMemoryBar(bars, 4, &low, &high));
    EXPECT_TRUE(low == 0xba000000u && high == 0u);
    bars[4] = 0;
    EXPECT_FALSE(PujjoReadMemoryBar(bars, 4, &low, &high));
    bars[4] = 4;
    EXPECT_TRUE(PujjoReadMemoryBar(bars, 4, &low, &high) && low == 0u && high == 1u);
    printf("PASS: %d Pujjo board identity and PCI BAR checks\n", checks);
    return 0;
}
