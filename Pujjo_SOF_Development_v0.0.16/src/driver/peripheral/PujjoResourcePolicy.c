/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoResourcePolicy.h"
#include <string.h>

enum pj_policy_result pj_validate_resources(enum pj_probe_kind device,
    const pj_resource *resources, size_t count, pj_summary *summary)
{
    size_t i, j;
    pj_summary s = {0};
    if (!summary) return PJ_POLICY_BAD_INPUT;
    memset(summary, 0, sizeof(*summary));
    if ((device != PJ_PROBE_CODEC && device != PJ_PROBE_AMP) ||
        (!resources && count)) return PJ_POLICY_BAD_INPUT;
    if (count == 0 || count > PJ_MAX_RESOURCES)
        return PJ_POLICY_EXCESSIVE_RESOURCES;
    for (i = 0; i < count; ++i) {
        const pj_resource *r = &resources[i];
        if (r->kind == PJ_RES_UNKNOWN_CONNECTION)
            return PJ_POLICY_UNEXPECTED_CONNECTION;
        if (r->kind == PJ_RES_OTHER) continue;
        if (r->kind == PJ_RES_INTERRUPT) {
            if (++s.interrupt_count > 1) return PJ_POLICY_RESOURCE_MISMATCH;
            continue;
        }
        if (r->kind != PJ_RES_I2C && r->kind != PJ_RES_GPIO_IO)
            return PJ_POLICY_UNEXPECTED_CONNECTION;
        /* A duplicated connection ID across different resource entries is
         * suspicious; never open any resources while this is unresolved. */
        for (j = 0; j < i; ++j) {
            if ((resources[j].kind == PJ_RES_I2C ||
                 resources[j].kind == PJ_RES_GPIO_IO) &&
                resources[j].id_low == r->id_low &&
                resources[j].id_high == r->id_high)
                return PJ_POLICY_DUPLICATE_ID;
        }
        if (r->kind == PJ_RES_I2C) ++s.i2c_count;
        if (r->kind == PJ_RES_GPIO_IO) ++s.gpio_count;
        s.connection_low = r->id_low;
        s.connection_high = r->id_high;
    }
    /* The headset codec's GPIO jack interrupt appears as an interrupt resource,
     * while the amplifier is a GPIO-only peripheral. An interrupt may be
     * absent at prepare-time, but an unexpected I2C/GPIO connection fails. */
    if (device == PJ_PROBE_CODEC) {
        if (s.i2c_count != 1 || s.gpio_count != 0)
            return PJ_POLICY_RESOURCE_MISMATCH;
    } else {
        if (s.i2c_count != 0 || s.gpio_count != 1 || s.interrupt_count != 0)
            return PJ_POLICY_RESOURCE_MISMATCH;
    }
    *summary = s;
    return PJ_POLICY_OK;
}
