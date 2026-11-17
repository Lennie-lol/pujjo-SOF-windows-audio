/* SPDX-License-Identifier: BSD-3-Clause */
/* Read-only identity/parent-path validation; no Windows kernel APIs or writes. */
#ifndef PUJJO_DEVICE_MAP_H
#define PUJJO_DEVICE_MAP_H

typedef enum {
    PJ_MAP_OK = 0, PJ_MAP_MISSING = 1, PJ_MAP_WRONG_BOARD = 2,
    PJ_MAP_WRONG_AUDIO_ID = 3, PJ_MAP_WRONG_CODEC = 4,
    PJ_MAP_WRONG_AMP = 5, PJ_MAP_WRONG_BUS = 6,
    PJ_MAP_WRONG_PATH = 7
} pj_map_result;

typedef struct {
    const char *manufacturer;
    const char *board;
    const char *controller_id; /* Device PCI instance ID */
    const char *codec_id;      /* ACPI instance ID */
    const char *amp_id;        /* ACPI instance ID */
    const char *codec_parent_id;
    const char *amp_parent_id;
    const char *codec_location_path; /* full ACPI path */
    const char *amp_location_path;
} pj_device_map;

/* Confirms ONLY the identity/topology of enumerated devices.
 * It does NOT prove I2C address, gpio polarity, availability, or speaker safety.
 * Real hardware access must remain disabled regardless of PJ_MAP_OK. */
pj_map_result pj_validate_device_map(const pj_device_map *map);
const char *pj_map_result_string(pj_map_result value);
#endif
