#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile full transport sources with fake WDF APIs; pthread close/I/O race on POSIX.
This deliberately is NOT Windows/WDK ABI or electrical validation.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='pj_wdf_') as d:
    tmp=Path(d)
    for name in ('ntddk.h','wdf.h','reshub.h','spb.h','gpio.h'):
        (tmp/name).write_text('#include "wdk_host_mock.h"\n')
    for lab in (False, True):
        exe=tmp/(('transport-lab' if lab else 'transport')+('.exe' if os.name=='nt' else ''))
        cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
             *shlex.split(os.environ.get('CFLAGS','')),
             *(['-pthread','-DPJ_HOST_THREADS'] if os.name=='posix' else []),
             *(['-DPJ5682S_ENABLE_LAB_COLD_INIT'] if lab else []),
             '-I'+str(tmp),'-I'+str(ROOT/'tests'),'-I'+str(ROOT/'src/driver/experimental'),
             str(ROOT/'tests/test_windows_transports.c'),'-o',str(exe)]
        print(' '.join(cmd),flush=True)
        subprocess.run(cmd,check=True,timeout=120)
        subprocess.run([str(exe)],check=True,timeout=30)
