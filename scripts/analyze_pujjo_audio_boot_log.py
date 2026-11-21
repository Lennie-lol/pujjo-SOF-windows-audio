#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Offline read-only triage of a captured Pujjo Linux boot log.

No privileged access, hardware reads, driver installation, or calibration.
A matching string is *not* proof that a codec, rail or clock is qualified.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

PATTERNS = {
    "sof_dsp": re.compile(r"\bsof(?:[-_ ]audio|[-_ ]pci|[-_ ]intel)?\b|snd[_-]sof", re.I),
    "rt5682s_codec": re.compile(r"\b(?:rt5682s?|rtl5682)\b", re.I),
    "rt1019_amp": re.compile(r"\b(?:rt1015p|rt1019p?|rtl1019)\b", re.I),
    "ssp": re.compile(r"\bssp[012]\b", re.I),
    "dmic": re.compile(r"\bdmic[01]?\b", re.I),
}
FAILURE = re.compile(r"\b(?:fail(?:ed|ure)?|error|timeout|timed out|probe deferred|i/o error)\b", re.I)
AUDIO_CONTEXT = re.compile(r"\b(?:i2c|gpio|acpi|snd|asoc|clock|power|reset|codec)\b", re.I)
TIMESTAMP = re.compile(r"^\s*\[\s*([0-9]+(?:\.[0-9]+)?)\]")

def analyze(text: str) -> dict:
    """Count related log evidence without asserting hardware readiness."""
    counts = {key: 0 for key in PATTERNS}
    errors = []
    related_line_numbers = []
    ordered = []
    for number, line in enumerate(text.splitlines(), 1):
        matched = [name for name, pattern in PATTERNS.items() if pattern.search(line)]
        if not matched:
            continue
        related_line_numbers.append(number)
        for name in matched:
            counts[name] += 1
        match = TIMESTAMP.match(line)
        ordered.append({"line": number, "seconds": float(match.group(1)) if match else None,
                        "signals": matched})
        if FAILURE.search(line) and AUDIO_CONTEXT.search(line):
            errors.append(number)
    return {
        "format": "pujjo-audio-boot-triage-v1",
        "lines_scanned": len(text.splitlines()),
        "related_lines": related_line_numbers,
        "signal_counts": counts,
        "possible_failure_lines": errors,
        "events": ordered,
        "unverified": [
            "codec rail/LDO power-cycle ownership and timing",
            "codec reset duration and electrical state",
            "actual MCLK/BCLK/FSYNC, SSP pinmux and PLL lock",
            "amplifier SDB polarity and physical output isolation",
            "real RT5682S calibration busy-to-idle register transitions",
            "Windows translated SPB/GPIO resource ownership"
        ],
        "hardware_writes_authorized": False,
        "cold_init_qualified": False
    }

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="Saved Linux dmesg text file (not a device path)")
    parser.add_argument("--json", action="store_true", help="Machine-readable counts and line numbers")
    args = parser.parse_args()
    report = analyze(args.log.read_text(encoding="utf-8", errors="replace"))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"Scanned {report['lines_scanned']} lines; matched {len(report['related_lines'])} audio-related lines.")
        for key, count in report["signal_counts"].items():
            print(f"  {key}: {count} lines")
        print(f"Possible failure line numbers: {report['possible_failure_lines']}")
        print("Result: diagnostic hints only. Hardware qualification and writes remain BLOCKED.")

if __name__ == "__main__":
    main()
