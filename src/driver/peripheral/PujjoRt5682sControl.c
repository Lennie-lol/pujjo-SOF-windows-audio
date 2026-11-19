/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoRt5682sControl.h"

/* RT5682S field encodings independently implemented from Linux v6.13
 * rt5682s.h and the register behavior in rt5682s.c. */
enum {
    RT5682S_CBJ_TRIGGER = 0x0010,
    RT5682S_TRIGGER_HIGH = 0x0008,
    RT5682S_CBJ_TYPE = 0x0011,
    RT5682S_STEREO_ADC_MIX = 0x0026,
    RT5682S_DAC1_MIX = 0x0029,
    RT5682S_DAC1_SOURCE = 0x002b,
    RT5682S_AIF1_POWER = 0x0061,
    RT5682S_ADDA_CLK = 0x0073,
    RT5682S_SYSCLK = 0x0080,
    RT5682S_AJD1 = 0x00f0,
    RT5682S_JACK_OUT = 0x0010
};

static pj5682s_status read_register(const pj5682s_bus *bus, PJ_WORD reg, PJ_WORD *value)
{
    PJ_BYTE address[2], data[2];
    pj5682s_encode_address(reg, address);
    if (bus->write_then_read(bus->context, address, sizeof(address), data, sizeof(data)))
        return PJ5682S_BUS_ERROR;
    *value = pj5682s_decode_value(data);
    return PJ5682S_OK;
}

static pj5682s_status verify_device(const pj5682s_bus *bus)
{
    PJ_WORD id;
    pj5682s_status status = read_register(bus, PJ5682S_DEVICE_ID_REGISTER, &id);
    if (status != PJ5682S_OK) return status;
    return id == PJ5682S_EXPECTED_DEVICE_ID ? PJ5682S_OK : PJ5682S_WRONG_CHIP;
}

static pj5682s_status update_register(const pj5682s_bus *bus, PJ_WORD reg,
                                     PJ_WORD mask, PJ_WORD value, PJ_WORD old)
{
    PJ_WORD command = (PJ_WORD)((old & (PJ_WORD)~mask) | value);
    PJ_WORD observed;
    PJ_BYTE data[4];
    if (command == old) return PJ5682S_OK;
    pj5682s_encode_write(reg, command, data);
    if (bus->write_only(bus->context, data, sizeof(data)) ||
        read_register(bus, reg, &observed) != PJ5682S_OK)
        return PJ5682S_BUS_ERROR;
    return (observed & mask) == value ? PJ5682S_OK : PJ5682S_VERIFY_FAILED;
}

static pj5682s_status apply(pj5682s_control *state, const pj5682s_bus *bus,
                           pj_clock_register_update update)
{
    PJ_WORD old;
    pj5682s_status status = read_register(bus, update.reg, &old);
    if (status != PJ5682S_OK) return status;
    if ((old & update.mask) == update.value) return PJ5682S_OK;
    if (state->count >= PJ5682S_MAX_SAVED_REGISTERS) return PJ5682S_FAULT;

    /* A failed transfer can still have changed the device. Journal it first. */
    pj5682s_saved_register *entry = &state->saved[state->count++];
    entry->reg = update.reg;
    entry->mask = update.mask;
    entry->original = (PJ_WORD)(old & update.mask);
    return update_register(bus, update.reg, update.mask, update.value, old);
}

pj5682s_status pj5682s_control_stop(pj5682s_control *state, const pj5682s_bus *bus)
{
    pj5682s_status first;
    unsigned remaining = 0;

    if (!state || !bus || !bus->write_then_read || !bus->write_only)
        return PJ5682S_BAD_ARGUMENT;
    state->active = 0;
    state->jd1_qualified = 0;
    if (!state->count) return PJ5682S_OK;
    first = verify_device(bus);
    if (first != PJ5682S_OK) {
        state->faulted = 1;
        return first;
    }

    /* Retry only the fields that could not be restored. Preserve their order
     * so a retry still unwinds the original writes in reverse. */
    for (unsigned i = state->count; i > 0; --i) {
        pj5682s_saved_register saved = state->saved[i - 1u];
        PJ_WORD current;
        pj5682s_status status = read_register(bus, saved.reg, &current);
        if (status == PJ5682S_OK)
            status = update_register(bus, saved.reg, saved.mask, saved.original, current);
        if (status != PJ5682S_OK) {
            if (first == PJ5682S_OK) first = status;
            state->saved[state->count - ++remaining] = saved;
        }
    }
    for (unsigned i = 0; i < remaining; ++i)
        state->saved[i] = state->saved[state->count - remaining + i];
    state->count = remaining;
    if (remaining) state->faulted = 1;
    return first;
}

pj5682s_status pj5682s_control_start(pj5682s_control *state, const pj5682s_bus *bus,
                                    const pj5682s_control_profile *profile)
{
    pj_rt5682s_i2s_plan framing;
    pj_clock_register_update updates[PJ5682S_MAX_SAVED_REGISTERS];
    unsigned count = 0;
    pj5682s_status status;

    if (!state || !bus || !bus->write_then_read || !bus->write_only || !profile)
        return PJ5682S_BAD_ARGUMENT;
    if (state->active || state->count || state->faulted) return PJ5682S_INCORRECT_STATE;
    if (profile->board_qualified != 1u || profile->analog_path_prepared_muted != 1u ||
        profile->dsp_muted != 1u || profile->jd1_qualified > 1u ||
        profile->mclk_hz != 24576000u || !profile->routes ||
        (profile->routes & ~(PJ5682S_ROUTE_DAC | PJ5682S_ROUTE_ADC)) ||
        pj_rt5682s_plan_i2s_receiver(profile->slot_bits, profile->bclk_hz,
            profile->lrclk_hz, profile->inverted_bclk, &framing) != PJ_CLK_OK)
        return PJ5682S_WRITE_LOCKED;

    status = verify_device(bus);
    if (status != PJ5682S_OK) return status;
    /* Leave an already owned AIF1 alone. The rest of the codec may have
     * independent board consumers. */
    PJ_WORD power;
    status = read_register(bus, RT5682S_AIF1_POWER, &power);
    if (status != PJ5682S_OK) return status;
    if (power & 0x8000u) return PJ5682S_INCORRECT_STATE;

    for (unsigned i = 0; i < framing.count; ++i) updates[count++] = framing.writes[i];
    updates[count++] = (pj_clock_register_update){RT5682S_SYSCLK, 0xe000, 0};
    updates[count++] = (pj_clock_register_update){RT5682S_ADDA_CLK, 0xf07f, 0x1001};
    if (profile->routes & PJ5682S_ROUTE_DAC) {
        /* Stereo DAC1 directly from the I2S mixer. Disable ADC loopback. */
        updates[count++] = (pj_clock_register_update){RT5682S_DAC1_MIX, 0xc0c0, 0x8080};
        updates[count++] = (pj_clock_register_update){RT5682S_DAC1_SOURCE, 0x0011, 0};
    }
    if (profile->routes & PJ5682S_ROUTE_ADC) {
        /* Select L/R ADC1 and disable ADC2 (digital mic or DAC loopback). */
        updates[count++] = (pj_clock_register_update){RT5682S_STEREO_ADC_MIX, 0xecec, 0x6064};
    }
    updates[count++] = (pj_clock_register_update){RT5682S_AIF1_POWER, 0x8000, 0x8000};
    for (unsigned i = 0; i < count; ++i) {
        status = apply(state, bus, updates[i]);
        if (status != PJ5682S_OK) {
            state->faulted = 1;
            (void)pj5682s_control_stop(state, bus);
            return status;
        }
    }
    state->active = 1;
    state->jd1_qualified = profile->jd1_qualified;
    return PJ5682S_OK;
}

pj5682s_status pj5682s_control_jack(const pj5682s_control *state, const pj5682s_bus *bus,
                                    unsigned detection_completed, pj5682s_jack *jack)
{
    PJ_WORD before, after, type = 0;
    pj5682s_status status;
    if (!state || !bus || !bus->write_then_read || !jack || detection_completed > 1u)
        return PJ5682S_BAD_ARGUMENT;
    if (!state->active || !state->jd1_qualified || state->faulted)
        return PJ5682S_INCORRECT_STATE;
    status = read_register(bus, RT5682S_AJD1, &before);
    if (status != PJ5682S_OK) return status;
    if (!(before & RT5682S_JACK_OUT) && detection_completed) {
        status = read_register(bus, RT5682S_CBJ_TYPE, &type);
        if (status != PJ5682S_OK) return status;
    }
    status = read_register(bus, RT5682S_AJD1, &after);
    if (status != PJ5682S_OK) return status;
    if ((before ^ after) & RT5682S_JACK_OUT) return PJ5682S_VERIFY_FAILED;
    type &= 3u;
    if (after & RT5682S_JACK_OUT) *jack = PJ5682S_JACK_ABSENT;
    else if (!type) *jack = PJ5682S_JACK_PRESENT_UNCLASSIFIED;
    else if (type == 3u) *jack = PJ5682S_JACK_HEADPHONE;
    else *jack = PJ5682S_JACK_HEADSET;
    return PJ5682S_OK;
}


pj5682s_status pj5682s_control_classify(pj5682s_control *state, const pj5682s_bus *bus,
    int (*delay_ms)(void *context, unsigned ms), pj5682s_jack *jack)
{
    PJ_WORD trigger, type;
    pj5682s_jack observed;
    pj5682s_status status;

    if (!state || !bus || !bus->write_then_read || !bus->write_only || !delay_ms || !jack)
        return PJ5682S_BAD_ARGUMENT;
    status = pj5682s_control_jack(state, bus, 0, &observed);
    if (status != PJ5682S_OK) return status;
    if (observed == PJ5682S_JACK_ABSENT) {
        *jack = observed;
        return PJ5682S_OK;
    }

    status = read_register(bus, RT5682S_CBJ_TRIGGER, &trigger);
    if (status != PJ5682S_OK) return status;
    unsigned i;
    for (i = 0; i < state->count; ++i)
        if (state->saved[i].reg == RT5682S_CBJ_TRIGGER) break;
    if (i == state->count) {
        if (i == PJ5682S_MAX_SAVED_REGISTERS) return PJ5682S_FAULT;
        state->saved[i] = (pj5682s_saved_register){
            RT5682S_CBJ_TRIGGER, RT5682S_TRIGGER_HIGH, (PJ_WORD)(trigger & RT5682S_TRIGGER_HIGH)
        };
        ++state->count;
    }
    status = update_register(bus, RT5682S_CBJ_TRIGGER, RT5682S_TRIGGER_HIGH, 0, trigger);
    if (status != PJ5682S_OK) return status;
    /* The RT5682S reference waits 45–50 ms between trigger edges, then
     * gives the detector up to 50 polls. Require a cleared old result. */
    if (delay_ms(bus->context, 45u)) return PJ5682S_BUS_ERROR;
    status = read_register(bus, RT5682S_CBJ_TYPE, &type);
    if (status != PJ5682S_OK) return status;
    if (type & 3u) return PJ5682S_VERIFY_FAILED;
    status = read_register(bus, RT5682S_CBJ_TRIGGER, &trigger);
    if (status != PJ5682S_OK) return status;
    status = update_register(bus, RT5682S_CBJ_TRIGGER, RT5682S_TRIGGER_HIGH, RT5682S_TRIGGER_HIGH, trigger);
    if (status != PJ5682S_OK) return status;
    for (i = 0; i < 50u; ++i) {
        if (delay_ms(bus->context, 10u)) return PJ5682S_BUS_ERROR;
        status = pj5682s_control_jack(state, bus, 1, &observed);
        if (status != PJ5682S_OK) return status;
        if (observed != PJ5682S_JACK_PRESENT_UNCLASSIFIED) {
            *jack = observed;
            return PJ5682S_OK;
        }
    }
    return PJ5682S_VERIFY_FAILED;
}
