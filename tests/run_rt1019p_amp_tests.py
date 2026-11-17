#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests only. Neither Windows WDK nor speaker hardware is used."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'src/driver/experimental/PujjoRt1019pAmp.c'
TEST=ROOT/'tests/test_rt1019p_amp.c'
CC=os.environ.get('CC') or next((c for c in ('clang','gcc','cc') if shutil.which(c)),None)
if not CC:
    raise SystemExit('Cannot run amplifier tests: no host C compiler installed')
with tempfile.TemporaryDirectory(prefix='rt1019p_amp_') as folder:
    exe=Path(folder)/('amp.exe' if os.name=='nt' else 'amp')
    for variant,flags in (
        ('production-locked',[]),
        ('test-double',['-DPJ1019_TEST_ONLY_CALLBACKS','-DPJ1019_TEST_BUILD']),
    ):
        subprocess.run([CC,'-std=c11','-Wall','-Wextra','-Werror',
                        '-Wconversion','-Wsign-conversion','-pedantic',
                        *flags,str(SRC),str(TEST),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=15)
        print('PASS:',variant)
