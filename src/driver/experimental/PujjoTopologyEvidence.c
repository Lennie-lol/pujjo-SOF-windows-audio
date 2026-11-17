/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoTopologyEvidence.h"
#include <string.h>

#define PJ_TPLG_HEADER_SIZE 36u
#define PJ_TPLG_MAGIC 0x41536f43u /* ALSA topology 'CoSA', little endian */
#define PJ_TPLG_MAX_SIZE (2u * 1024u * 1024u)
#define PJ_TPLG_MAX_SECTIONS 512u

static uint32_t get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Strict NUL-bounded token match, not a loose substring search inside a large blob. */
static int contains_token(const uint8_t *data, size_t size, const char *token)
{
    size_t i;
    size_t n = strlen(token);
    if (n > size) return 0;
    for (i = 0; i <= size - n; ++i) {
        if (memcmp(data + i, token, n) != 0) continue;
        if (i && data[i - 1] != '\0') continue;
        if (i + n < size && data[i + n] != '\0') continue;
        return 1;
    }
    return 0;
}

pj_topology_result pj_inspect_adl_topology(const uint8_t *data, size_t size,
                                           pj_topology_evidence *out)
{
    size_t at = 0;
    unsigned count = 0;
    pj_topology_evidence evidence = {0};
    if (!data || !out || size < PJ_TPLG_HEADER_SIZE || size > PJ_TPLG_MAX_SIZE)
        return PJ_TPLG_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    while (at < size) {
        uint32_t header_size, payload_size, abi;
        const uint8_t *payload;
        size_t remaining = size - at;
        if (remaining < PJ_TPLG_HEADER_SIZE) return PJ_TPLG_TRUNCATED;
        if (get_le32(data + at) != PJ_TPLG_MAGIC) return PJ_TPLG_BAD_HEADER;
        abi = get_le32(data + at + 4);
        header_size = get_le32(data + at + 16);
        payload_size = get_le32(data + at + 24);
        /* Verify bounds before accessing payload to prevent integer overflow. */
        if (abi == 0 || header_size != PJ_TPLG_HEADER_SIZE)
            return PJ_TPLG_BAD_HEADER;
        if ((size_t)header_size > remaining ||
            (size_t)payload_size > remaining - (size_t)header_size)
            return PJ_TPLG_TRUNCATED;
        payload = data + at + (size_t)header_size;
        if (contains_token(payload, payload_size, "SSP1.OUT") &&
            contains_token(payload, payload_size, "SSP1-Codec"))
            evidence.has_ssp1_speaker_playback = 1;
        if (contains_token(payload, payload_size, "SSP0.OUT") &&
            contains_token(payload, payload_size, "SSP0-Codec"))
            evidence.has_ssp0_headset_playback = 1;
        if (contains_token(payload, payload_size, "SSP0.IN") &&
            contains_token(payload, payload_size, "SSP0-Codec"))
            evidence.has_ssp0_headset_capture = 1;
        if (contains_token(payload, payload_size, "SSP2.OUT") &&
            contains_token(payload, payload_size, "SSP2-BT"))
            evidence.has_ssp2_bt_playback = 1;
        if (contains_token(payload, payload_size, "SSP2.IN") &&
            contains_token(payload, payload_size, "SSP2-BT"))
            evidence.has_ssp2_bt_capture = 1;
        if (contains_token(payload, payload_size, "DMIC0.IN") ||
            contains_token(payload, payload_size, "DMIC1.IN"))
            evidence.has_dmic_capture = 1;
        at += (size_t)header_size + (size_t)payload_size;
        if (++count > PJ_TPLG_MAX_SECTIONS) return PJ_TPLG_BAD_HEADER;
    }
    evidence.section_count = count;
    if (!evidence.has_ssp1_speaker_playback || !evidence.has_ssp0_headset_playback ||
        !evidence.has_ssp0_headset_capture || !evidence.has_ssp2_bt_playback ||
        !evidence.has_ssp2_bt_capture) return PJ_TPLG_MISSING_ROUTE;
    *out = evidence;
    return PJ_TPLG_VALID;
}
