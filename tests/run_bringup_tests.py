#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile portable bring-up coordinator and run locked/mocked scenarios."""
import os
import shutil
import shlex
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'src' / 'driver' / 'experimental'
CC = os.environ.get('CC') or next((c for c in ('clang', 'gcc', 'cc') if shutil.which(c)), None)
if not CC:
    raise SystemExit('No host C compiler found')
source = [
    EXP / 'PujjoBringupCoordinator.c', EXP / 'PujjoAudioInit.c',
    EXP / 'PujjoRt1019pAmp.c', EXP / 'PujjoSofFirmwarePolicy.c',
    ROOT / 'tests' / 'test_pujjo_bringup.c',
]
with tempfile.TemporaryDirectory(prefix='pj_bringup_') as tmp:
    for name, flags in (
        ('production-locked', []),
        ('host-only-mock', ['-DPJ_BRINGUP_TEST_CALLBACKS', '-DPJ_BRINGUP_HOST_TEST']),
    ):
        exe = Path(tmp) / ('bringup.exe' if os.name == 'nt' else 'bringup')
        cmd = [CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
               '-Wsign-conversion', '-pedantic', *shlex.split(os.environ.get('CFLAGS','')), *flags, *map(str, source), '-o', str(exe)]
        subprocess.run(cmd, check=True, timeout=35)
        subprocess.run([str(exe)], check=True, timeout=12)
        print('PASS', name)

# Compile-time fail-closed test: accidental kernel opt-in MUST be rejected.
with tempfile.TemporaryDirectory(prefix='pj_bringup_guard_') as tmp:
    proc = subprocess.run([CC, '-std=c11', '-DPJ_BRINGUP_TEST_CALLBACKS',
                           '-DPJ_BRINGUP_HOST_TEST', '-D_KERNEL_MODE',
                           '-c', str(EXP / 'PujjoBringupCoordinator.c'),
                           '-o', str(Path(tmp) / 'unsafe.o')],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode != 0 and 'host tests ONLY' in proc.stderr, (
        'Kernel guard regression: expected deliberate compile-time rejection', proc.stderr
    )
    print('PASS kernel-mode opt-in correctly rejected by compiler')
