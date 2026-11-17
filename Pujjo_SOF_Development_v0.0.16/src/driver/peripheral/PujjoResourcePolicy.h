/* SPDX-License-Identifier: BSD-3-Clause */
/* Pure-C policy. No platform dependencies and absolutely no hardware writes. */
#ifndef PUJJO_RESOURCE_POLICY_H
#define PUJJO_RESOURCE_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum pj_probe_kind { PJ_PROBE_CODEC = 1, PJ_PROBE_AMP = 2 };
enum pj_resource_kind {
    PJ_RES_OTHER = 0, PJ_RES_I2C = 1, PJ_RES_GPIO_IO = 2,
    PJ_RES_INTERRUPT = 3, PJ_RES_UNKNOWN_CONNECTION = 4
};
enum pj_policy_result {
    PJ_POLICY_OK = 0, PJ_POLICY_BAD_INPUT = 1,
    PJ_POLICY_EXCESSIVE_RESOURCES = 2, PJ_POLICY_UNEXPECTED_CONNECTION = 3,
    PJ_POLICY_RESOURCE_MISMATCH = 4, PJ_POLICY_DUPLICATE_ID = 5
};
#define PJ_MAX_RESOURCES 64u
/* The connection IDs are opaque Windows Resource Hub identifiers, NOT physical
 * GPIO pin numbers or I2C addresses. Never use them for register writes. */
typedef struct pj_resource {
    uint32_t kind;
    uint32_t id_low;
    uint32_t id_high;
} pj_resource;
typedef struct pj_summary {
    uint32_t i2c_count;
    uint32_t gpio_count;
    uint32_t interrupt_count;
    uint32_t connection_low;
    uint32_t connection_high;
} pj_summary;
enum pj_policy_result pj_validate_resources(enum pj_probe_kind device,
    const pj_resource *resources, size_t count, pj_summary *summary);
#ifdef __cplusplus
}
#endif
#endif
