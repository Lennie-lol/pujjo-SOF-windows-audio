/* SPDX-License-Identifier: BSD-3-Clause */
/* Portable RT5682S I2C control boundary. Not a functioning Windows audio driver.
 * All write operations are TEST-BUILD-ONLY; shipping builds cannot write.
 * The chipset is RT5682S (RTL5682 ACPI HID), not the older RT5682. */
#ifndef PUJJO_RT5682S_CODEC_H
#define PUJJO_RT5682S_CODEC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PJ5682S_DEVICE_ID_REGISTER 0x00ffu
#define PJ5682S_EXPECTED_DEVICE_ID 0x6749u
#define PJ5682S_VENDOR_ID_REGISTER 0x00feu
#define PJ5682S_VERSION_REGISTER 0x00fdu
#define PJ5682S_I2C_ADDR_7BIT 0x1au
#define PJ5682S_I2C_SPEED_HZ 400000u

typedef enum pj5682s_status {
    PJ5682S_OK=0, PJ5682S_BAD_ARGUMENT=-1, PJ5682S_NO_BUS=-2,
    PJ5682S_BUS_ERROR=-3, PJ5682S_WRONG_CHIP=-4,
    PJ5682S_NOT_IDENTIFIED=-5, PJ5682S_SUSPENDED=-6,
    PJ5682S_WRITE_LOCKED=-7, PJ5682S_INCORRECT_STATE=-8,
    PJ5682S_VERIFY_FAILED=-9, PJ5682S_FAULT=-10
} pj5682s_status;

typedef enum pj5682s_state {
    PJ5682S_CLOSED=0, PJ5682S_BOUND=1, PJ5682S_IDENTIFIED=2,
    PJ5682S_ASLEEP=3, PJ5682S_FAILED=4
} pj5682s_state;

/* Atomic write-then-read is required for the 16-bit indexed I2C reads.
 * Return zero on success, nonzero on failure. The physical I2C address and
 * bus speed must be provided by Windows Resource Hub, NOT this structure. */
typedef struct pj5682s_bus {
    void *context;
    int (*write_then_read)(void *context, const uint8_t *tx, size_t tx_size,
                           uint8_t *rx, size_t rx_size);
    int (*write_only)(void *context, const uint8_t *tx, size_t tx_size);
} pj5682s_bus;

typedef struct pj5682s_codec {
    pj5682s_bus bus;
    pj5682s_state state;
    uint16_t verified_id;
    uint32_t successful_read_count;
    uint32_t successful_test_write_count;
} pj5682s_codec;

/* No bus I/O until pj5682s_identify(). Bind must come only after independent
 * Pujjo board, RTL5682 PnP ID, and translated I2C resource validation. */
pj5682s_status pj5682s_bind(pj5682s_codec *c, const pj5682s_bus *bus);
pj5682s_status pj5682s_identify(pj5682s_codec *c);
/* Public diagnostics intentionally permit ONLY identity registers, to avoid
 * side-effectful reads of arbitrary codec control/status registers. */
pj5682s_status pj5682s_read_identity(pj5682s_codec *c,
                                      uint16_t register_address,
                                      uint16_t *value);
pj5682s_status pj5682s_suspend(pj5682s_codec *c);
pj5682s_status pj5682s_resume(pj5682s_codec *c);
void pj5682s_unbind(pj5682s_codec *c);

/* Exact wire encoding of 16-bit register and 16-bit value. These are not
 * implicitly executed; they can be validated without any I2C device. */
void pj5682s_encode_address(uint16_t reg, uint8_t out[2]);
void pj5682s_encode_write(uint16_t reg, uint16_t value, uint8_t out[4]);
uint16_t pj5682s_decode_value(const uint8_t bytes[2]);

/* This exists for unit tests of register-update semantics only. It is
 * physically impossible to compile into a production WDK build without the
 * explicit PJ5682S_TEST_ONLY_WRITES macro. Never define that macro on device. */
pj5682s_status pj5682s_test_update_bits(pj5682s_codec *c,
        uint16_t reg, uint16_t mask, uint16_t value);

#ifdef __cplusplus
}
#endif
#endif
