/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoAcpiResources.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    pj_acpi_audio_resources known = {
        0x1Au, 400000u, 87u, 75u, 0,
        "\\_SB.PCI0.I2C3", "\\_SB.PCI0.GPIO", "\\_SB.PCI0.GPIO"
    };
    pj_acpi_audio_resources e;
    assert(pj_check_acpi_evidence(&known) == PJ_ACPI_OK);
    assert(pj_check_acpi_evidence(NULL) == PJ_ACPI_MISSING);
    e = known; e.codec_i2c_address = 0x1Bu;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_CODEC_RESOURCE_MISMATCH);
    e = known; e.codec_i2c_speed_hz = 100000u;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_CODEC_RESOURCE_MISMATCH);
    e = known; e.codec_irq_gpio = 0u;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_CODEC_RESOURCE_MISMATCH);
    e = known; e.amp_sdb_gpio = 0u;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_AMP_RESOURCE_MISMATCH);
    e = known; e.amp_has_i2c_resource = 1;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_AMP_RESOURCE_MISMATCH);
    e = known; e.codec_i2c_controller = "\\_SB.PCI0.I2C2";
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_CONTROLLER_MISMATCH);
    e = known; e.amp_gpio_controller = NULL;
    assert(pj_check_acpi_evidence(&e) == PJ_ACPI_MISSING);
    puts("PASS: 9 Pujjo ACPI evidence checks");
    return 0;
}
