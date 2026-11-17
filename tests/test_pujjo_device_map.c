/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include "../src/driver/experimental/PujjoDeviceMap.h"

int main(void) {
    const pj_device_map good = {
        "Google", "Pujjo",
        "PCI\\VEN_8086&DEV_54C8&SUBSYS_00000000&REV_00\\3&11583659&0&FB",
        "ACPI\\RTL5682\\0", "ACPI\\RTL1019\\0",
        "PCI\\VEN_8086&DEV_54EB&SUBSYS_72708086&REV_00\\3&11583659&0&AB",
        "PCI\\VEN_8086&DEV_54EB&SUBSYS_72708086&REV_00\\3&11583659&0&AB",
        "ACPI(_SB_)#ACPI(PCI0)#ACPI(I2C3)#ACPI(RT58)",
        "ACPI(_SB_)#ACPI(PCI0)#ACPI(I2C3)#ACPI(ALCP)"
    };
    pj_device_map changed = good;
    assert(pj_validate_device_map(&good) == PJ_MAP_OK);
    changed.board = "Pujjo1e"; assert(pj_validate_device_map(&changed) == PJ_MAP_OK);
    changed.board = "Other"; assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_BOARD);
    changed = good; changed.controller_id = "PCI\\VEN_8086&DEV_1234&";
    assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_AUDIO_ID);
    changed = good; changed.codec_id = "ACPI\\RTL0000\\0";
    assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_CODEC);
    changed = good; changed.amp_id = "ACPI\\RTL0000\\0";
    assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_AMP);
    changed = good; changed.amp_parent_id = "PCI\\VEN_8086&DEV_54EA&XX";
    assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_BUS);
    changed = good; changed.codec_location_path = "ACPI(_SB_)#ACPI(PCI0)#ACPI(I2C2)#ACPI(RT58)";
    assert(pj_validate_device_map(&changed) == PJ_MAP_WRONG_PATH);
    changed = good; changed.codec_parent_id = NULL;
    assert(pj_validate_device_map(&changed) == PJ_MAP_MISSING);
    assert(pj_validate_device_map(NULL) == PJ_MAP_MISSING);
    printf("PASS: 10 device-map cases; no device I/O performed.\\n");
    return 0;
}
