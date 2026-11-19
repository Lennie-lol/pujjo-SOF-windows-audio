#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run the production register transaction engine with a host-only codec."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    out = Path(temp) / ("control.exe" if os.name == "nt" else "control")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-pedantic", *shlex.split(os.environ.get("CFLAGS", ""))]
    command += [str(root / path) for path in (
        "src/driver/peripheral/PujjoRt5682sControl.c",
        "src/driver/experimental/PujjoRt5682sCodec.c",
        "src/driver/experimental/PujjoHeadsetClock.c",
        "tests/test_pujjo_codec_control.c",
    )] + ["-o", str(out)]
    subprocess.run(command, check=True, timeout=40)
    subprocess.run([str(out)], check=True, timeout=20)
print("PASS: RT5682S control I/O, qualified routes, jack consistency and recoverable rollback")
