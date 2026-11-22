#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Offline SSP candidate arithmetic checks; never proof of measured board clocks.

These are reviewed candidate configurations, not clock-programming values.
Do not infer PLL lock, physical MCLK, pad routing, or safe amplifier activation.
"""
from dataclasses import dataclass, replace


@dataclass(frozen=True)
class SspCandidate:
    port: int
    rate_hz: int
    channels: int
    slot_bits: int
    sample_bits: int
    bclk_hz: int
    reference_hz: int
    divider: int


def validate(candidate: SspCandidate) -> bool:
    """Fail closed on unsupported geometry or contradictory integer divider."""
    if candidate.port not in (0, 1):
        return False
    if candidate.rate_hz != 48_000 or candidate.channels != 2:
        return False
    if candidate.sample_bits != 24 or candidate.slot_bits < candidate.sample_bits:
        return False
    if candidate.slot_bits != (25 if candidate.port == 0 else 32):
        return False
    if any(x <= 0 for x in (candidate.bclk_hz, candidate.reference_hz, candidate.divider)):
        return False
    return (candidate.rate_hz * candidate.channels * candidate.slot_bits == candidate.bclk_hz
            and candidate.reference_hz == candidate.bclk_hz * candidate.divider)


def main() -> None:
    # From the audited decoded SSP profile arithmetic, not hardware measurements.
    headset = SspCandidate(0, 48_000, 2, 25, 24, 2_400_000, 38_400_000, 16)
    speakers = SspCandidate(1, 48_000, 2, 32, 24, 3_072_000, 24_576_000, 8)
    assert validate(headset) and validate(speakers)
    assert headset.bclk_hz != 48_000 * 2 * 24  # 25-bit physical slots, 24-bit samples
    for good in (headset, speakers):
        assert not validate(replace(good, bclk_hz=good.bclk_hz + 1))
        assert not validate(replace(good, reference_hz=good.reference_hz + 1))
        assert not validate(replace(good, divider=0))
        assert not validate(replace(good, sample_bits=16))
        assert not validate(replace(good, channels=1))
        assert not validate(replace(good, rate_hz=44_100))
        assert not validate(replace(good, port=2))
    assert not validate(replace(headset, slot_bits=32, bclk_hz=3_072_000))
    assert not validate(replace(speakers, slot_bits=25, bclk_hz=2_400_000))
    print('PASS: SSP0/SSP1 candidate arithmetic and fail-closed geometry (offline only)')


if __name__ == '__main__':
    main()
