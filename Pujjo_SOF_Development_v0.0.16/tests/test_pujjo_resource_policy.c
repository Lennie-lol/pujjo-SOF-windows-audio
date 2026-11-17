/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoResourcePolicy.h"
#include <assert.h>
#include <stdio.h>

static void expect(enum pj_probe_kind kind, const pj_resource *r, size_t n,
                   enum pj_policy_result outcome) {
    pj_summary s = {0};
    assert(pj_validate_resources(kind, r, n, &s) == outcome);
    if (outcome != PJ_POLICY_OK) assert(s.i2c_count == 0 && s.gpio_count == 0);
}
int main(void) {
    pj_resource good_codec[] = {{PJ_RES_I2C,1,0},{PJ_RES_INTERRUPT,0,0}};
    pj_resource good_amp[] = {{PJ_RES_GPIO_IO,2,0}};
    pj_resource bad_kind[] = {{PJ_RES_UNKNOWN_CONNECTION,1,0}};
    pj_resource mixed[] = {{PJ_RES_I2C,1,0},{PJ_RES_GPIO_IO,2,0}};
    pj_resource duplicate[] = {{PJ_RES_I2C,1,0},{PJ_RES_GPIO_IO,1,0}};
    pj_resource no_resources[] = {{PJ_RES_OTHER,0,0}};
    pj_resource too_many[65] = {{PJ_RES_I2C,1,0}};
    pj_summary s;
    expect(PJ_PROBE_CODEC, good_codec, 2, PJ_POLICY_OK);
    expect(PJ_PROBE_AMP, good_amp, 1, PJ_POLICY_OK);
    expect(PJ_PROBE_CODEC, bad_kind, 1, PJ_POLICY_UNEXPECTED_CONNECTION);
    expect(PJ_PROBE_CODEC, mixed, 2, PJ_POLICY_RESOURCE_MISMATCH);
    expect(PJ_PROBE_AMP, mixed, 2, PJ_POLICY_RESOURCE_MISMATCH);
    expect(PJ_PROBE_AMP, duplicate, 2, PJ_POLICY_DUPLICATE_ID);
    expect(PJ_PROBE_CODEC, no_resources, 1, PJ_POLICY_RESOURCE_MISMATCH);
    expect(PJ_PROBE_CODEC, good_codec, 0, PJ_POLICY_EXCESSIVE_RESOURCES);
    expect(PJ_PROBE_CODEC, too_many, 65, PJ_POLICY_EXCESSIVE_RESOURCES);
    expect(PJ_PROBE_CODEC, good_codec, 2, PJ_POLICY_OK);
    assert(pj_validate_resources(PJ_PROBE_CODEC, good_codec, 2, NULL) == PJ_POLICY_BAD_INPUT);
    assert(pj_validate_resources(PJ_PROBE_CODEC, NULL, 1, &s) == PJ_POLICY_BAD_INPUT);
    assert(pj_validate_resources((enum pj_probe_kind)42, good_codec, 2, &s) == PJ_POLICY_BAD_INPUT);
    assert(pj_validate_resources(PJ_PROBE_CODEC, good_codec, 2, &s) == PJ_POLICY_OK);
    assert(s.i2c_count == 1 && s.gpio_count == 0 && s.interrupt_count == 1);
    assert(pj_validate_resources((enum pj_probe_kind)42, good_codec, 2, &s) == PJ_POLICY_BAD_INPUT);
    assert(s.connection_low == 0 && s.i2c_count == 0 && s.interrupt_count == 0);
    assert(pj_validate_resources(PJ_PROBE_CODEC, good_codec, 2, &s) == PJ_POLICY_OK);
    assert(pj_validate_resources(PJ_PROBE_CODEC, NULL, 1, &s) == PJ_POLICY_BAD_INPUT);
    assert(s.connection_low == 0 && s.i2c_count == 0 && s.interrupt_count == 0);
    puts("PASS: resource policy test cases including stale-output rejection");
    return 0;
}
