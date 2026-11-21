#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests for the non-authorizing Pujjo boot-log triage."""
import importlib.util
from pathlib import Path

root = Path(__file__).resolve().parents[1]
path = root / "scripts" / "analyze_pujjo_audio_boot_log.py"
spec = importlib.util.spec_from_file_location("pujjo_audio_log_triage", path)
module = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(module)

log = """[    0.100] random unrelated USB driver loaded
[    1.000] sof-audio-pci-intel-tgl: DSP firmware loaded
[    1.010] rt5682s i2c-INT34C3:00: codec detected
[    1.020] rt1015p RTL1019: sound amplifier probe
[    1.030] ASoC: SSP0-Codec connected
[    1.040] sof-audio-pci-intel-tgl: IPC timeout
[    1.050] dmIc0: candidate capture path
"""
report = module.analyze(log)
assert report["lines_scanned"] == 7
assert report["related_lines"] == [2, 3, 4, 5, 6, 7]
assert report["signal_counts"] == {
    "sof_dsp": 2,
    "rt5682s_codec": 1,
    "rt1019_amp": 1,
    "ssp": 1,
    "dmic": 1,
}
assert report["possible_failure_lines"] == [6]
assert report["events"][0] == {"line": 2, "seconds": 1.0, "signals": ["sof_dsp"]}
assert report["cold_init_qualified"] is False
assert report["hardware_writes_authorized"] is False
assert "amplifier SDB polarity and physical output isolation" in report["unverified"]

# Evidence absence and garbled input cannot accidentally certify hardware.
for text in ("", "boot succeeded", "[garbled]"):
    result = module.analyze(text)
    assert not result["related_lines"] and not result["possible_failure_lines"]
    assert not result["cold_init_qualified"] and not result["hardware_writes_authorized"]

print("PASS: offline Pujjo audio log triage never authorizes hardware writes")
