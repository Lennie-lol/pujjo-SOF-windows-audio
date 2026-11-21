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
    RT5682S_PLL_POWER = 0x0065,
    RT5682S_PLLB_SUPPLIES = 0x5020,
    RT5682S_PLLB_RESET = 0x0080,
    RT5682S_I2S2_CLOCK = 0x00a0,
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
                           pj_clock_register_update update, PJ_WORD restore_ms)
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
    entry->restore_ms = restore_ms;
    return update_register(bus, update.reg, update.mask, update.value, old);
}

pj5682s_status pj5682s_control_stop(pj5682s_control *state, const pj5682s_bus *bus)
{
    pj5682s_status first;

    if (!state || !bus || !bus->write_then_read || !bus->write_only)
        return PJ5682S_BAD_ARGUMENT;
    state->active = 0;
    state->jd1_qualified = 0;
    if (!state->count) {
        state->wait = 0;
        return PJ5682S_OK;
    }
    first = verify_device(bus);
    if (first != PJ5682S_OK) {
        state->faulted = 1;
        return first;
    }

    /* Strict LIFO rollback: an earlier write can overlap a later field.
     * Restoring it while the later entry is pending would replay an
     * intermediate value on retry. Keep the complete older prefix until
     * the failed entry is restored, and never discard a failed write. */
    while (state->count) {
        const pj5682s_saved_register *saved = &state->saved[state->count - 1u];
        PJ_WORD current;
        pj5682s_status status = read_register(bus, saved->reg, &current);
        if (status == PJ5682S_OK)
            status = update_register(bus, saved->reg, saved->mask, saved->original, current);
        if (status == PJ5682S_OK && saved->restore_ms &&
            (!state->wait || state->wait(bus->context, saved->restore_ms)))
            status = PJ5682S_BUS_ERROR;
        if (status != PJ5682S_OK) {
            state->faulted = 1;
            return status;
        }
        --state->count;
    }
    state->wait = 0;
    return PJ5682S_OK;
}

pj5682s_status pj5682s_control_start_clocked(pj5682s_control *state, const pj5682s_bus *bus,
    const pj5682s_control_profile *profile, pj5682s_wait clock_wait)
{
    pj_rt5682s_i2s_plan framing;
    pj_rt5682s_pll2_plan pll = {0};
    pj_clock_register_update updates[PJ5682S_MAX_START_WRITES];
    unsigned count = 0, settle_after = 0;
    unsigned use_pll;
    pj5682s_status status;

    if (!state || !bus || !bus->write_then_read || !bus->write_only || !profile)
        return PJ5682S_BAD_ARGUMENT;
    if (state->active || state->count || state->faulted) return PJ5682S_INCORRECT_STATE;
    if (profile->board_qualified != 1u || profile->analog_path_prepared_muted != 1u ||
        profile->dsp_muted != 1u || profile->jd1_qualified > 1u ||
        !profile->routes ||
        (profile->routes & ~(PJ5682S_ROUTE_DAC | PJ5682S_ROUTE_ADC)) ||
        pj_rt5682s_plan_i2s_receiver(profile->slot_bits, profile->bclk_hz,
            profile->lrclk_hz, profile->inverted_bclk, &framing) != PJ_CLK_OK)
        return PJ5682S_WRITE_LOCKED;

    use_pll = profile->mclk_hz != 24576000u;
    if (use_pll) {
        if (profile->mclk_hz != 19200000u || profile->pll2_qualified != 1u ||
            !profile->pll2_settle_ms || profile->pll2_settle_ms > 500u || !clock_wait ||
            pj_rt5682s_plan_pll2(profile->mclk_hz, 24576000u, &pll) != PJ_CLK_OK)
            return PJ5682S_WRITE_LOCKED;
    } else if (profile->pll2_qualified || profile->pll2_settle_ms) {
        return PJ5682S_WRITE_LOCKED;
    }

    status = verify_device(bus);
    if (status != PJ5682S_OK) return status;
    /* Leave an already owned AIF1 alone. The rest of the codec may have
     * independent board consumers. */
    PJ_WORD power;
    status = read_register(bus, RT5682S_AIF1_POWER, &power);
    if (status != PJ5682S_OK) return status;
    if (power & 0x8000u) return PJ5682S_INCORRECT_STATE;

    if (use_pll) {
        /* Refuse to reconfigure PLLB if another part of the codec owns it.
         * Power, source and reset fields follow Linux v6.13 rt5682s. */
        PJ_WORD source;
        status = read_register(bus, RT5682S_PLL_POWER, &power);
        if (status != PJ5682S_OK) return status;
        if (power & (RT5682S_PLLB_SUPPLIES | RT5682S_PLLB_RESET))
            return PJ5682S_INCORRECT_STATE;
        status = read_register(bus, RT5682S_SYSCLK, &source);
        if (status != PJ5682S_OK) return status;
        if ((source & 0xe000u) == 0x4000u) return PJ5682S_INCORRECT_STATE;
        status = read_register(bus, RT5682S_ADDA_CLK, &source);
        if (status != PJ5682S_OK) return status;
        if ((source & 0x0070u) == 0x0020u) return PJ5682S_INCORRECT_STATE;
        status = read_register(bus, RT5682S_I2S2_CLOCK, &source);
        if (status != PJ5682S_OK) return status;
        if ((source & 0x0070u) == 0x0020u) return PJ5682S_INCORRECT_STATE;
    }

    for (unsigned i = 0; i < framing.count; ++i) updates[count++] = framing.writes[i];
    if (use_pll) {
        for (unsigned i = 0; i < pll.count; ++i) updates[count++] = pll.writes[i];
        updates[count++] = (pj_clock_register_update){RT5682S_PLL_POWER,
            RT5682S_PLLB_SUPPLIES, RT5682S_PLLB_SUPPLIES};
        updates[count++] = (pj_clock_register_update){RT5682S_PLL_POWER,
            RT5682S_PLLB_RESET, RT5682S_PLLB_RESET};
        settle_after = count;
    }
    updates[count++] = (pj_clock_register_update){RT5682S_SYSCLK, 0xe000,
        (PJ_WORD)(use_pll ? 0x4000u : 0u)};
    updates[count++] = (pj_clock_register_update){RT5682S_ADDA_CLK, 0xf07f,
        (PJ_WORD)(use_pll ? 0x1021u : 0x1001u)};
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
        status = apply(state, bus, updates[i], 0);
        if (status == PJ5682S_OK && i + 1u == settle_after &&
            clock_wait(bus->context, profile->pll2_settle_ms))
            status = PJ5682S_BUS_ERROR;
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

pj5682s_status pj5682s_control_start(pj5682s_control *state, const pj5682s_bus *bus,
                                    const pj5682s_control_profile *profile)
{
    return pj5682s_control_start_clocked(state, bus, profile, 0);
}

/* Register and mask pairs follow the v6.13 RT5682S DAPM power,
 * depop, mixer and combo-jack fields. Values and ordering belong to
 * the measured Pujjo board profile. */
PJ_WORD pj5682s_analog_mask(PJ_WORD reg)
{
    switch (reg) {
    case 0x0003: return 0x6001; /* HPO path and input gating */
    case 0x0005:
    case 0x0006: return 0x0f00; /* HPO gain */
    case 0x0010: return 0x0030; /* MB1/MB2 control, no trigger */
    case 0x0012: return 0x0080; /* Combo-jack input buffer */
    case 0x0019: return 0xffff; /* Digital DAC gain */
    case 0x001c: return 0xffff; /* Digital ADC gain and mute */
    case 0x002a: return 0xf0f0; /* Stereo DAC mixer */
    case 0x003c: return 0x3fc0; /* REC mixer and CBJ boost */
    case 0x0044: return 0x0101; /* REC mixer power */
    case 0x0061: return 0x0c18; /* DAC1/ADC1 power, no AIF writes */
    case 0x0062: return 0x8400; /* Stereo DAC/ADC filter power */
    case 0x0063: return 0xf280; /* VREF, fast VREF, MB and BG */
    case 0x0064: return 0x0c01; /* Mic bias and JD power */
    case 0x0065: return 0x0207; /* CBJ and bias LDOs */
    case 0x0066: return 0x0030; /* Stereo DAC power */
    case 0x008e: return 0x007b; /* Depop, HP pumps and outputs */
    case 0x0091: return 0x0f00; /* HPO switches and charge-pump mode */
    case 0x0093: return 0xc300; /* MB voltage fields */
    case 0x0210: return 0xc320; /* SAR mode and bias selection */
    case 0x0212: return 0xffff; /* SAR detection operating setting */
    case 0x021c: return 0x003f; /* SAR source */
    case 0x0610:
    case 0x0611: return 0xffff; /* HP bias from qualified depop plan */
    case 0x3b00: return 0x0070; /* Charge-pump switch size */
    default: return 0;
    }
}

pj5682s_status pj5682s_validate_analog(const pj5682s_analog_program *program)
{
    unsigned total = 0;
    if (!program || !program->count || program->count > PJ5682S_MAX_ANALOG_STEPS)
        return PJ5682S_BAD_ARGUMENT;
    for (unsigned i = 0; i < program->count; ++i) {
        const pj5682s_analog_step *step = &program->steps[i];
        if (!step->mask || (step->mask & ~pj5682s_analog_mask(step->reg)) ||
            (step->value & ~step->mask) || step->settle_ms > 100u || step->restore_ms > 100u)
            return PJ5682S_WRITE_LOCKED;
        total += step->settle_ms + step->restore_ms;
    }
    return total <= 500u ? PJ5682S_OK : PJ5682S_WRITE_LOCKED;
}

pj5682s_status pj5682s_control_analog(pj5682s_control *state, const pj5682s_bus *bus,
    const pj5682s_analog_program *program, pj5682s_wait wait)
{
    pj5682s_status status;
    if (!state || !bus || !bus->write_then_read || !bus->write_only || !wait)
        return PJ5682S_BAD_ARGUMENT;
    if (!state->active || state->faulted || state->wait) return PJ5682S_INCORRECT_STATE;
    status = pj5682s_validate_analog(program);
    if (status != PJ5682S_OK) return status;
    state->wait = wait;
    for (unsigned i = 0; i < program->count; ++i) {
        const pj5682s_analog_step *step = &program->steps[i];
        pj_clock_register_update update = {step->reg, step->mask, step->value};
        status = apply(state, bus, update, step->restore_ms);
        if (status == PJ5682S_OK && step->settle_ms && wait(bus->context, step->settle_ms))
            status = PJ5682S_BUS_ERROR;
        if (status != PJ5682S_OK) {
            state->faulted = 1;
            (void)pj5682s_control_stop(state, bus);
            return status;
        }
    }
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
        if (state->saved[i].reg == RT5682S_CBJ_TRIGGER &&
            state->saved[i].mask == RT5682S_TRIGGER_HIGH) break;
    if (i == state->count) {
        if (i == PJ5682S_MAX_SAVED_REGISTERS) return PJ5682S_FAULT;
        state->saved[i] = (pj5682s_saved_register){
            RT5682S_CBJ_TRIGGER, RT5682S_TRIGGER_HIGH, (PJ_WORD)(trigger & RT5682S_TRIGGER_HIGH), 0
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

/* RT5682S cold-start candidate, register facts cross-checked against
 * Linux v6.13 sound/soc/codecs/rt5682s.{c,h}. This independent executor
 * fails closed on every transfer and requires a fresh busy-to-idle edge.
 * It must not be used for resume or an already-owned streaming device. */
typedef struct {
    PJ_WORD reg, mask, value;
    unsigned delay_ms;
} pj5682s_cold_op;

/* Zero mask is command-only: reset, calibration trigger and volatile SAR
 * configuration have no stable readback contract. All other entries
 * verify the selected bits; full-width writes require exclusive ownership. */
static pj5682s_status cold_op(const pj5682s_bus *bus, const pj5682s_cold_op *op,
                              pj5682s_wait wait)
{
    PJ_WORD value = op->value, observed;
    PJ_BYTE bytes[4];
    pj5682s_status status;
    if (op->mask && op->mask != 0xffffu) {
        status = read_register(bus, op->reg, &observed);
        if (status != PJ5682S_OK) return status;
        value = (PJ_WORD)((observed & (PJ_WORD)~op->mask) | value);
    }
    pj5682s_encode_write(op->reg, value, bytes);
    if (bus->write_only(bus->context, bytes, sizeof(bytes))) return PJ5682S_BUS_ERROR;
    if (op->delay_ms && wait(bus->context, op->delay_ms)) return PJ5682S_BUS_ERROR;
    if (!op->mask) return PJ5682S_OK;
    status = read_register(bus, op->reg, &observed);
    if (status != PJ5682S_OK) return status;
    return (observed & op->mask) == op->value ? PJ5682S_OK : PJ5682S_VERIFY_FAILED;
}

static pj5682s_status cold_batch(const pj5682s_bus *bus, pj5682s_wait wait,
    const pj5682s_cold_op *ops, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        pj5682s_status status = cold_op(bus, &ops[i], wait);
        if (status != PJ5682S_OK) return status;
    }
    return PJ5682S_OK;
}

#define COLD_WRITE(r,v) {r,0xffffu,v,0}
#define COLD_COUNT(a) ((unsigned)(sizeof(a)/sizeof((a)[0])))
static const pj5682s_cold_op cold_reset = {0,0,0,1};
static const pj5682s_cold_op cold_patch[] = {
    COLD_WRITE(0x0008,0x0007), COLD_WRITE(0x0132,0),
    COLD_WRITE(0x013a,0x2020), COLD_WRITE(0x0118,0x0101),
    COLD_WRITE(0x0117,0x80c0), COLD_WRITE(0x01e6,2),
    COLD_WRITE(0x008e,0), COLD_WRITE(0x0091,0x3c15),
    COLD_WRITE(0x0019,0xfefe), {0x0211,0,0xac00,0},
    {0x0212,0,0x024c,0}, COLD_WRITE(0x0015,0x0804),
    {0x0062,0x0080,0,20}
};
static const pj5682s_cold_op cold_prepare[] = {
    {0x0063,0xffff,0xaa80,15}, COLD_WRITE(0x0063,0xfa80),
    COLD_WRITE(0x0061,0x01c0), COLD_WRITE(0x0094,0x0380),
    COLD_WRITE(0x0080,0x8000), COLD_WRITE(0x0073,0x1001),
    COLD_WRITE(0x013a,0x3030), COLD_WRITE(0x013b,0xb000),
    COLD_WRITE(0x0026,0x686c), COLD_WRITE(0x0044,0x5151),
    COLD_WRITE(0x01df,0x0321), COLD_WRITE(0x01db,4),
    {0x01de,0,0x7c00,0}
};
static const pj5682s_cold_op cold_trigger = {0x01de,0,0xfc00,0};
static const pj5682s_cold_op cold_restore[] = {
    COLD_WRITE(0x0094,0x0180), COLD_WRITE(0x0044,0x5858),
    COLD_WRITE(0x0026,0xc0c4), COLD_WRITE(0x01df,0x0320),
    COLD_WRITE(0x0061,0x00c0), COLD_WRITE(0x0063,0x0800),
    COLD_WRITE(0x0080,0),
    {0x0094,0x0300,0x0100,0}, {0x0063,0x0080,0x0080,0},
    {0x01db,0x0003,0x0003,0}, {0x0091,0x0300,0x0200,0},
    {0x3b00,0x0070,0x0020,0}
};
#undef COLD_WRITE

pj5682s_status pj5682s_cold_initialize(pj5682s_cold_state *state,
    const pj5682s_bus *bus, const pj5682s_cold_profile *profile, pj5682s_wait wait)
{
    pj5682s_status status;
    PJ_WORD sample;
    if (!state || !bus || !bus->write_then_read || !bus->write_only || !profile || !wait)
        return PJ5682S_BAD_ARGUMENT;
    if (state->phase != PJ5682S_COLD_IDLE) return PJ5682S_INCORRECT_STATE;
    if (profile->board_qualified != 1u || profile->exclusive_owner != 1u ||
        profile->rails_ready != 1u || profile->outputs_isolated != 1u)
        return PJ5682S_WRITE_LOCKED;

    state->phase = PJ5682S_COLD_SETTLING;
    status = wait(bus->context, 50u) ? PJ5682S_BUS_ERROR : verify_device(bus);
    if (status != PJ5682S_OK) goto failed;
    state->phase = PJ5682S_COLD_RESET;
    state->reset_attempted = 1;
    status = cold_op(bus, &cold_reset, wait);
    if (status != PJ5682S_OK) goto failed;
    status = verify_device(bus);
    if (status != PJ5682S_OK) goto failed;
    state->phase = PJ5682S_COLD_PATCH;
    status = cold_batch(bus, wait, cold_patch, COLD_COUNT(cold_patch));
    if (status != PJ5682S_OK) goto failed;
    state->phase = PJ5682S_COLD_PREPARE;
    status = cold_batch(bus, wait, cold_prepare, COLD_COUNT(cold_prepare));
    if (status != PJ5682S_OK) goto failed;
    status = read_register(bus, 0x01ea, &sample);
    if (status != PJ5682S_OK) goto failed;
    if (sample & 0x8000u) { status = PJ5682S_VERIFY_FAILED; goto failed; }

    state->phase = PJ5682S_COLD_CALIBRATING;
    status = cold_op(bus, &cold_trigger, wait);
    if (status != PJ5682S_OK) goto failed;
    while (state->polls < 60u) {
        ++state->polls;
        status = read_register(bus, 0x01ea, &sample);
        if (status != PJ5682S_OK) goto failed;
        if (sample & 0x8000u) state->busy_seen = 1;
        else if (state->busy_seen) break;
        if (state->polls == 60u) { status = PJ5682S_VERIFY_FAILED; goto failed; }
        if (wait(bus->context, 10u)) { status = PJ5682S_BUS_ERROR; goto failed; }
    }

    state->phase = PJ5682S_COLD_RESTORE;
    status = cold_batch(bus, wait, cold_restore, COLD_COUNT(cold_restore));
    if (status != PJ5682S_OK) goto failed;
    status = verify_device(bus);
    if (status != PJ5682S_OK) goto failed;
    state->phase = PJ5682S_COLD_READY;
    return PJ5682S_OK;

failed:
    state->failed_at = state->phase;
    state->phase = PJ5682S_COLD_FAILED;
    /* A transfer can fail after changing the chip. Do not retry calibration,
     * or try to restore arbitrary pre-reset state. Keep external isolation
     * asserted and attempt a reset only after re-verifying identity. */
    if (state->reset_attempted) {
        state->recovery_status = verify_device(bus);
        if (state->recovery_status == PJ5682S_OK) {
            state->recovery_status = cold_op(bus, &cold_reset, wait);
            /* A successful bus write is not evidence that reset restored
             * the expected codec. Keep the original failure latched and
             * report recovery success only after a fresh identity read. */
            if (state->recovery_status == PJ5682S_OK)
                state->recovery_status = verify_device(bus);
        }
    }
    return status;
}
#undef COLD_COUNT

/* Portable power-epoch guard. Never equate D0Entry with fresh calibration. */
void pj5682s_cold_invalidate(pj5682s_cold_state *state)
{
    if (!state) return;
    state->failed_at = state->phase;
    state->phase = PJ5682S_COLD_FAILED;
}

int pj5682s_cold_is_ready(const pj5682s_cold_state *state)
{
    return state && state->phase == PJ5682S_COLD_READY;
}
