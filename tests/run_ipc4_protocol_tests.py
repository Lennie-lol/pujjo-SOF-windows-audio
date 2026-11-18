#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile IPC4 source with the same portable C implementation WDK compiles.
This test never talks to the DSP or Windows PnP manager.
"""
from pathlib import Path
import os
import shutil
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
cc=os.environ.get('CC') or next((v for v in ('clang','gcc','cc') if shutil.which(v)),None)
if not cc:
    raise SystemExit('FAIL: no host C compiler for IPC4 validation')
sources=[ROOT/'src/driver/PujjoIpc4Protocol.c',
         ROOT/'src/driver/PujjoIpc4Transaction.c',
         ROOT/'tests/test_pujjo_ipc4_protocol.c']
with tempfile.TemporaryDirectory(prefix='pj_ipc4_protocol_') as temp:
    out=Path(temp)/('ipc4.exe' if os.name=='nt' else 'ipc4')
    for name,defs in (('production-locked',[]),('mock-transaction',['-DPJ_IPC4_TRANSACTION_HOST_TEST'])):
        cmd=[cc,'-std=c11','-Wall','-Wextra','-Werror',
             '-Wconversion','-Wsign-conversion','-pedantic',*defs,*shlex.split(os.environ.get('CFLAGS','')),
             *map(str,sources),'-o',str(out)]
        subprocess.run(cmd,check=True,timeout=40)
        subprocess.run([str(out)],check=True,timeout=20)
        print('PASS:',name)
    forbidden=subprocess.run([cc,'-std=c11','-D_KERNEL_MODE',
                              '-DPJ_IPC4_TRANSACTION_HOST_TEST',
                              '-c',str(ROOT/'src/driver/PujjoIpc4Transaction.c'),
                              '-o',str(Path(temp)/'forbidden.o')],
                             text=True,capture_output=True,timeout=20)
    assert forbidden.returncode!=0
    assert 'forbidden in WDK kernels' in forbidden.stderr
    print('PASS: kernel transaction mock opt-in rejected')
