/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoAcpiResources.h"
#include <string.h>

pj_acpi_check pj_check_acpi_evidence(const pj_acpi_audio_resources *e) {
    if (!e || !e->codec_i2c_controller || !e->codec_gpio_controller ||
        !e->amp_gpio_controller) return PJ_ACPI_MISSING;
    if (e->codec_i2c_address != 0x1Au || e->codec_i2c_speed_hz != 400000u ||
        e->codec_irq_gpio != 87u) return PJ_ACPI_CODEC_RESOURCE_MISMATCH;
    if (e->amp_sdb_gpio != 75u || e->amp_has_i2c_resource)
        return PJ_ACPI_AMP_RESOURCE_MISMATCH;
    if (strcmp(e->codec_i2c_controller, "\\_SB.PCI0.I2C3") != 0 ||
        strcmp(e->codec_gpio_controller, "\\_SB.PCI0.GPIO") != 0 ||
        strcmp(e->amp_gpio_controller, "\\_SB.PCI0.GPIO") != 0)
        return PJ_ACPI_CONTROLLER_MISMATCH;
    return PJ_ACPI_OK;
}
