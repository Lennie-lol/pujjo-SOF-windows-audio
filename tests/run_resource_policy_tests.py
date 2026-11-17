#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run previously unconnected resource policy unit tests, without hardware."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
    exe=Path(d)/('resources.exe' if os.name=='nt' else 'resources')
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
        '-I'+str(root/'src/driver/peripheral'),str(root/'src/driver/peripheral/PujjoResourcePolicy.c'),
        str(root/'tests/test_pujjo_resource_policy.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)
