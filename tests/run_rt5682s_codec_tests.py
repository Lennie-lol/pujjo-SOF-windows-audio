#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Portable codec mock tests, no kernel driver installation or real I2C."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
src = root / 'src/driver/experimental/PujjoRt5682sCodec.c'
test = root / 'tests/test_rt5682s_codec.c'
compiler = os.environ.get('CC') or next(
    (c for c in ('clang', 'gcc', 'cc') if shutil.which(c)), None)
if not compiler:
    print('SKIP: no C compiler found; tests not run (install compiler only on build host)')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rt5682s_codec_') as folder:
    output = Path(folder)/('codec.exe' if os.name == 'nt' else 'codec')
    for variant, extra in (('locked', []), ('mock-write-only', ['-DPJ5682S_TEST_ONLY_WRITES'])):
        cmd = [compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
               '-Wconversion', '-Wsign-conversion', '-pedantic', *extra,
               str(src), str(test), '-o', str(output)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(output)], check=True, timeout=15)
        print('PASS: '+variant)
print('PASS: both isolated RT5682S C suites')
