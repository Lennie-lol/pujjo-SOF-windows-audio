/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoDeviceMap.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int equal_case_insensitive(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}
static int begins_case_insensitive(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        ++s; ++prefix;
    }
    return 1;
}
static int contains_case_insensitive(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 0;
    while (*s) {
        if (begins_case_insensitive(s, needle)) return 1;
        ++s;
    }
    return 0;
}

pj_map_result pj_validate_device_map(const pj_device_map *m) {
    if (!m || !m->manufacturer || !m->board || !m->controller_id ||
        !m->codec_id || !m->amp_id || !m->codec_parent_id ||
        !m->amp_parent_id || !m->codec_location_path || !m->amp_location_path)
        return PJ_MAP_MISSING;
    if (!equal_case_insensitive(m->manufacturer, "Google") ||
        !(equal_case_insensitive(m->board, "Pujjo") ||
          equal_case_insensitive(m->board, "Pujjo1e")))
        return PJ_MAP_WRONG_BOARD;
    if (!begins_case_insensitive(m->controller_id, "PCI\\VEN_8086&DEV_54C8&"))
        return PJ_MAP_WRONG_AUDIO_ID;
    if (!begins_case_insensitive(m->codec_id, "ACPI\\RTL5682\\"))
        return PJ_MAP_WRONG_CODEC;
    if (!begins_case_insensitive(m->amp_id, "ACPI\\RTL1019\\"))
        return PJ_MAP_WRONG_AMP;
    /* Intel I2C3 controller identified in Windows PnP output as DEV_54EB.
     * This is a shared parent *controller*, NOT evidence of codec I2C addresses. */
    if (!begins_case_insensitive(m->codec_parent_id, "PCI\\VEN_8086&DEV_54EB&") ||
        !begins_case_insensitive(m->amp_parent_id, "PCI\\VEN_8086&DEV_54EB&") ||
        !equal_case_insensitive(m->codec_parent_id, m->amp_parent_id))
        return PJ_MAP_WRONG_BUS;
    if (!contains_case_insensitive(m->codec_location_path,
            "ACPI(PCI0)#ACPI(I2C3)#ACPI(RT58)") ||
        !contains_case_insensitive(m->amp_location_path,
            "ACPI(PCI0)#ACPI(I2C3)#ACPI(ALCP)"))
        return PJ_MAP_WRONG_PATH;
    return PJ_MAP_OK;
}
const char *pj_map_result_string(pj_map_result v) {
    switch (v) {
      case PJ_MAP_OK: return "enumerated identity verified (resources unknown)";
      case PJ_MAP_MISSING: return "missing evidence";
      case PJ_MAP_WRONG_BOARD: return "unsupported board";
      case PJ_MAP_WRONG_AUDIO_ID: return "wrong Intel controller";
      case PJ_MAP_WRONG_CODEC: return "wrong codec";
      case PJ_MAP_WRONG_AMP: return "wrong amplifier";
      case PJ_MAP_WRONG_BUS: return "I2C controller mismatch";
      case PJ_MAP_WRONG_PATH: return "ACPI location mismatch";
      default: return "unknown result";
    }
}
