#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Kernel-type source path tested with *fake* WDK declarations and host compilers.

Validates portable-code/kernel-branch type compatibility, NOT an actual WDK build.
No installation, system changes or hardware I/O.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
fixtures = (
    ('board', 'src/driver/PujjoBoardId.c', 'tests/test_pujjo_boardid.c'),
    ('resources', 'src/driver/peripheral/PujjoResourcePolicy.c',
     'tests/test_pujjo_resource_policy.c'),
    ('codec', 'src/driver/experimental/PujjoRt5682sCodec.c',
     'tests/test_rt5682s_codec.c'),
    ('clock', 'src/driver/experimental/PujjoHeadsetClock.c',
     'tests/test_pujjo_headset_clock.c'),
)

with tempfile.TemporaryDirectory(prefix='pj_fake_km_') as tmp_dir:
    tmp = Path(tmp_dir)
    # The fake kernel header deliberately avoids importing any desktop CRT.
    (tmp / 'ntddk.h').write_text('''
#ifndef PJ_TEST_NTDDK_H
#define PJ_TEST_NTDDK_H
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned int ULONG;
typedef __SIZE_TYPE__ SIZE_T;
#define RtlZeroMemory(p,n) __builtin_memset((p),0,(n))
#define RtlCopyMemory(d,s,n) __builtin_memcpy((d),(s),(n))
#endif
''')
    for cc_name in ('gcc', 'clang'):
        cc = shutil.which(cc_name)
        if cc is None:
            raise RuntimeError(f'{cc_name} not installed, cannot complete all available tests')
        for label, src, test in fixtures:
            wrapped = tmp / f'{label}_test.c'
            wrapped.write_text('#include <stdint.h>\n#include <stddef.h>\n'
                               f'#include "{(root / test).as_posix()}"\n')
            binary = tmp / f'{cc_name}_{label}'
            cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-O2', '-D_KERNEL_MODE', '-I' + str(tmp),
                   '-I' + str(root / 'src/driver/peripheral'),
                   str(root / src), str(wrapped), '-o', str(binary)]
            compiled = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
            if compiled.returncode:
                raise RuntimeError(f'Kernel mock compile failed {cc_name}/{label}: '+compiled.stdout+compiled.stderr)
            subprocess.run([str(binary)], check=True, capture_output=True,
                           text=True, timeout=30)
            print(f'PASS {cc_name}: fake kernel-type {label} build + unit tests', flush=True)

    # The Windows-targeting Clang front end catches Microsoft-ABI syntax errors.
    # It does not link or run against the real WDK.
    clang = shutil.which('clang')
    for label, src, _ in fixtures:
        cmd = [clang, '--target=x86_64-pc-windows-msvc', '-x', 'c',
               '-std=c11', '-fsyntax-only', '-Werror', '-Wall', '-Wextra',
               '-D_KERNEL_MODE', '-I' + str(tmp), str(root / src)]
        subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=40)
        print(f'PASS clang-msvc-target: fake kernel-type {label} syntax', flush=True)

    # This explicit negative test is essential: the codec mock writes must be
    # unrepresentable in an actual kernel-mode build.
    for cc_name in ('gcc', 'clang'):
        cmd = [shutil.which(cc_name), '-std=c11', '-D_KERNEL_MODE',
               '-DPJ5682S_TEST_ONLY_WRITES', '-I' + str(tmp), '-fsyntax-only',
               str(root / fixtures[2][1])]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        if result.returncode == 0 or 'forbidden in a Windows kernel build' not in result.stderr:
            raise RuntimeError('Test-only RT5682S writer was not blocked: ' + result.stderr)
        print(f'PASS {cc_name}: negative test rejects kernel test-only writes', flush=True)

print('PASS: fake WDK type path tests complete; actual MSVC/WDK still required')
