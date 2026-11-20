#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fixed Pujjo recovery geometry candidate, independent of the ALSA output.

These values are a review proposal for cAVS 2.5 IPC4 v1.5, not a driver
enablement decision. Keep ACS as the reference encoder emits it until the
reserved-bit review is complete. Source-selector mapping is corroborated by
Zephyr v4.2.0 drivers/dai/intel/ssp/{ssp.c,ssp.h} and cavs DTS clock nodes.
The exact running firmware and its M/N/reset state are still unverified.
"""
import argparse
import json
from pathlib import Path
import struct
from prepare_pujjo_ssp_candidate import build, decode, digest

ALSA_REV = "f04b9e0f1285fe25d4146906c5a8511741b8abad"
RECOVERY_SHA = "9ca138df2d471e9da3699566d2131a56cb9a0bc3274b074e7cc78e668ff5c697"


def candidate(port):
    if port not in (0, 1):
        raise ValueError("not an approved review port")
    slot, source, divider = (25, 0, 16) if port == 0 else (32, 1, 8)
    # SSP0/1 use normal-polarity I2S, DSP-provided clocks, two 24-bit samples.
    # MCLK is 38.4 MHz / 2. SSP1 BCLK uses the 24.576 MHz cardinal source.
    ssc0 = 0xc1d00077 | ((divider - 1) << 8)
    ssc1 = 0xd0700000
    padding = slot - 24
    sspsp = (1 << 25) | (slot << 16) | ((padding & 3) << 23) | ((padding >> 2) << 26)
    words = [0, 0xee000105, 96] + [0xffffff10] * 8
    words += [ssc0, ssc1, 0, sspsp, 3, 3, 0x4002, 0, 0x07070000, 0x20,
              1 | (source << 20), 1, 0]
    blob = struct.pack("<24I", *words)
    decode(blob, port)
    return blob


def audit_clock_plan(blob, port):
    """Decode clock selectors and validate exact integral recovery geometry.

    This is conditional on unity M/N and the reference cAVS clock source
    mapping. The running Intel-signed DSP's state is not inferred.
    """
    d = decode(blob, port)
    clocks = {0: 38400000, 1: 24576000}
    control = d["mdivctlr"]
    mclk_source = (control >> 16) & 3
    bclk_source = (control >> 20) & 3
    if control != (1 | (port << 20)) or d["mdivr"] != 0:
        raise ValueError("unexpected MCLK/BCLK source or divider")
    if not d["ssc0"] & (1 << 6):
        raise ValueError("external clock selection absent")
    scr = ((d["ssc0"] >> 8) & 0xfff) + 1
    slot = (d["sspsp"] >> 16) & 0x3f
    padding = ((d["sspsp"] >> 23) & 3) | (((d["sspsp"] >> 26) & 7) << 2)
    if slot != 24 + padding or clocks[bclk_source] != 48000 * 2 * slot * scr:
        raise ValueError("BCLK does not match frame geometry")
    if clocks[mclk_source] != 19200000 * (d["mdivr"] + 2):
        raise ValueError("MCLK does not match requested rate")
    return {"port": port, "mclk_selector": mclk_source, "bclk_selector": bclk_source,
            "scr_divisor": scr, "mclk_hz": 19200000,
            "bclk_hz": 48000 * 2 * slot, "slot_bits": slot,
            "conditional_on_unverified_unity_mn": True,
            "acs_set": bool(d["ssc0"] & (1 << 30)),
            "hardware_qualified": False}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference0", type=Path)
    p.add_argument("reference1", type=Path)
    p.add_argument("output", type=Path)
    args = p.parse_args()
    r0, r1 = (x.read_bytes() for x in (args.reference0, args.reference1))
    a, b = candidate(0), candidate(1)
    nhlt, report = build(r0, r1, a, b)
    report.update({"encoder_revision_expected": ALSA_REV,
                   "recovery_topology_sha256": RECOVERY_SHA,
                   "candidate_ssp0_sha256": digest(a), "candidate_ssp1_sha256": digest(b),
                   "clock_plan": {"mclk_source": "XTAL 38.4 MHz", "mclk_hz": 19200000,
                                  "ssp0_bclk_hz": 2400000, "ssp1_bclk_hz": 3072000},
                   "decoded_clock_audit": [audit_clock_plan(a, 0), audit_clock_plan(b, 1)],
                   "reservation": "ACS retained pending running-firmware and reserved-bit review"})
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in (("ssp0-candidate.bin", a), ("ssp1-candidate.bin", b),
                       ("candidate-only.nhlt", nhlt)):
        (args.output / name).write_bytes(data)
    (args.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: fixed candidate compared with upstream encoder; review only, no hardware approval")


if __name__ == "__main__":
    main()
