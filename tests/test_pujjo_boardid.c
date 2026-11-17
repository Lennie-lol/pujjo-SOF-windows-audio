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
    printf("PASS: %d Pujjo board identification checks\n", checks);
    return 0;
}
