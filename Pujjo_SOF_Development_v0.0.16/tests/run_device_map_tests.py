#!/usr/bin/env python3
"""Compile read-only Pujjo device-map checks with a host C compiler."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
compiler = os.environ.get("CC", "cc")
with tempfile.TemporaryDirectory(prefix="pujjo_map_") as d:
    exe = str(Path(d) / "test_device_map")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        str(root / "src/driver/experimental/PujjoDeviceMap.c"),
        str(root / "tests/test_pujjo_device_map.c"), "-o", exe], check=True)
    subprocess.run([exe], check=True)
print("PASS: device-map test build and execution")
