/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/peripheral/PujjoRt5682sControl.h"
#include <assert.h>
#include <string.h>

typedef struct {
    PJ_WORD regs[256];
    unsigned reads, writes;
    unsigned fail_first_write, fail_following_writes;
    unsigned unplug_on_second_sample;
    unsigned delay_count, keep_stale_type;
} fake_codec;

static int read_bus(void *user, const PJ_BYTE *tx, PJ_SIZE len, PJ_BYTE *rx, PJ_SIZE size)
{
    fake_codec *chip = user;
    PJ_WORD reg = pj5682s_decode_value(tx);
    assert(len == 2 && size == 2 && reg < 256);
    ++chip->reads;
    pj5682s_encode_address(chip->regs[reg], rx);
    if (reg == 0x00f0 && chip->unplug_on_second_sample) chip->regs[reg] = 0x10;
    return 0;
}

static int write_bus(void *user, const PJ_BYTE *tx, PJ_SIZE size)
{
    fake_codec *chip = user;
    PJ_WORD reg = pj5682s_decode_value(tx);
    assert(size == 4 && reg < 256 && reg != PJ5682S_DEVICE_ID_REGISTER);
    ++chip->writes;
    if (chip->fail_following_writes && chip->writes > chip->fail_first_write)
        return -1;
    chip->regs[reg] = pj5682s_decode_value(tx + 2);
    return chip->writes == chip->fail_first_write ? -1 : 0;
}

static int codec_delay(void *user, unsigned ms)
{
    fake_codec *chip = user;
    ++chip->delay_count;
    if (ms == 45 && !chip->keep_stale_type) {
        assert(!(chip->regs[0x10] & 8u));
        chip->regs[0x11] = 0;
    } else if (ms == 10 && chip->delay_count == 3) {
        assert(chip->regs[0x10] & 8u);
        chip->regs[0x11] = 1;
    }
    return 0;
}

int main(void)
{
    pj5682s_control control = {0};
    pj5682s_control_profile profile = {
        1,1,1,1,24576000,3072000,48000,32,0,PJ5682S_ROUTE_DAC | PJ5682S_ROUTE_ADC
    };
    fake_codec chip = {0};
    pj5682s_bus bus = {&chip,read_bus,write_bus};
    pj5682s_jack jack = PJ5682S_JACK_ABSENT;

    profile.board_qualified = 0;
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_WRITE_LOCKED);
    assert(!chip.reads && !chip.writes);
    profile.board_qualified = 1;
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_WRONG_CHIP);
    chip.regs[0xff] = PJ5682S_EXPECTED_DEVICE_ID;
    chip.regs[0x61] = 0x8123;
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_INCORRECT_STATE);
    assert(!chip.writes);
    chip.regs[0x61] = 0x0123;
    chip.regs[0x70] = 0x8808;
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_OK);
    assert(chip.regs[0x61] == 0x8123 && chip.regs[0x70] == 0xbb28);
    assert(chip.regs[0x29] == 0x8080 && chip.regs[0x26] == 0x6064);
    chip.regs[0x11] = 1;
    assert(pj5682s_control_jack(&control,&bus,0,&jack) == PJ5682S_OK);
    assert(jack == PJ5682S_JACK_PRESENT_UNCLASSIFIED);
    assert(pj5682s_control_jack(&control,&bus,1,&jack) == PJ5682S_OK && jack == PJ5682S_JACK_HEADSET);
    chip.regs[0x10] = 0x4008;
    chip.keep_stale_type = 1;
    assert(pj5682s_control_classify(&control,&bus,codec_delay,&jack) == PJ5682S_VERIFY_FAILED);
    assert(chip.delay_count == 1 && chip.regs[0x10] == 0x4000);
    chip.delay_count = 0;
    chip.keep_stale_type = 0;
    assert(pj5682s_control_classify(&control,&bus,codec_delay,&jack) == PJ5682S_OK);
    assert(chip.delay_count == 3 && jack == PJ5682S_JACK_HEADSET);

    chip.unplug_on_second_sample = 1;
    assert(pj5682s_control_jack(&control,&bus,1,&jack) == PJ5682S_VERIFY_FAILED);
    assert(jack == PJ5682S_JACK_HEADSET);
    assert(pj5682s_control_jack(&control,&bus,1,&jack) == PJ5682S_OK && jack == PJ5682S_JACK_ABSENT);

    chip.regs[0x70] &= (PJ_WORD)~0x8000u; /* independent bit changes survive stop */
    assert(pj5682s_control_stop(&control,&bus) == PJ5682S_OK);
    assert(chip.regs[0x61] == 0x0123 && chip.regs[0x70] == 0x0808 && !control.count);
    assert(chip.regs[0x10] == 0x4008);
    assert(pj5682s_control_jack(&control,&bus,0,&jack) == PJ5682S_INCORRECT_STATE);

    chip.fail_first_write = chip.writes + 2;
    chip.fail_following_writes = 1;
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_BUS_ERROR);
    assert(control.faulted && !control.active && control.count);
    unsigned writes = chip.writes;
    chip.regs[0xff] = 0;
    assert(pj5682s_control_stop(&control,&bus) == PJ5682S_WRONG_CHIP && chip.writes == writes);
    chip.regs[0xff] = PJ5682S_EXPECTED_DEVICE_ID;
    chip.fail_following_writes = 0;
    assert(pj5682s_control_stop(&control,&bus) == PJ5682S_OK && !control.count);
    assert(pj5682s_control_start(&control,&bus,&profile) == PJ5682S_INCORRECT_STATE);
    return 0;
}
