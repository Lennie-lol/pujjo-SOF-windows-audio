/* SPDX-License-Identifier: BSD-3-Clause */
/* Evidence validation only: NOT included in kernel driver, performs no I/O. */
#ifndef PUJJO_ACPI_RESOURCES_H
#define PUJJO_ACPI_RESOURCES_H

typedef struct pj_acpi_audio_resources {
    unsigned codec_i2c_address;
    unsigned codec_i2c_speed_hz;
    unsigned codec_irq_gpio;
    unsigned amp_sdb_gpio;
    int amp_has_i2c_resource;
    const char *codec_i2c_controller;
    const char *codec_gpio_controller;
    const char *amp_gpio_controller;
} pj_acpi_audio_resources;

typedef enum pj_acpi_check {
    PJ_ACPI_OK = 0,
    PJ_ACPI_MISSING = 1,
    PJ_ACPI_CODEC_RESOURCE_MISMATCH = 2,
    PJ_ACPI_AMP_RESOURCE_MISMATCH = 3,
    PJ_ACPI_CONTROLLER_MISMATCH = 4
} pj_acpi_check;

/* Compares read-only decoded evidence; never causes hardware access.
 * A PJ_ACPI_OK result is NOT an authorization to enable speaker power. */
pj_acpi_check pj_check_acpi_evidence(const pj_acpi_audio_resources *e);

#endif
