#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host-only fail-closed DSP boot contract tests; never invoke kernel or firmware."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC') or next((x for x in ('clang', 'gcc', 'cc') if shutil.which(x)), None)
if not cc:
    raise SystemExit('FAIL: C compiler required for IPC4 boot contract tests')
source = root / 'src/driver/experimental/PujjoIpc4BootModel.c'
test = root / 'tests/test_pujjo_ipc4_boot_model.c'
with tempfile.TemporaryDirectory(prefix='pj_ipc4_model_') as temp:
    target = Path(temp) / ('boot.exe' if os.name == 'nt' else 'boot')
    for mode, flags in (('locked', []), ('mock', ['-DPJ_IPC4_HOST_TEST'])):
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
               '-Wsign-conversion', '-pedantic', *flags, str(source), str(test), '-o', str(target)]
        subprocess.run(cmd, check=True, timeout=40)
        subprocess.run([str(target)], check=True, timeout=10)
        print('PASS:', mode, 'IPC4 boot state transitions')
    out = Path(temp) / 'forbidden.o'
    blocked = subprocess.run([cc, '-std=c11', '-D_KERNEL_MODE', '-DPJ_IPC4_HOST_TEST',
                              '-c', str(source), '-o', str(out)],
                             capture_output=True, text=True, timeout=20)
    assert blocked.returncode != 0 and 'must never be enabled in a kernel build' in blocked.stderr
    print('PASS: kernel-mode mock opt-in correctly rejected')
