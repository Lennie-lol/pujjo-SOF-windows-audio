#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host-only C tests; no WDK, chipset, firmware image or physical device used."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
cc=os.environ.get('CC') or next((s for s in ('clang','gcc','cc') if shutil.which(s)),None)
if not cc: raise SystemExit('Host C compiler not found')
with tempfile.TemporaryDirectory(prefix='pujjo_sof_policy_') as d:
    exe=Path(d)/('test.exe' if os.name=='nt' else 'test')
    subprocess.run([cc,'-std=c11','-pedantic','-Wall','-Wextra','-Werror',
                    '-Wconversion','-Wsign-conversion',
                    str(root/'src/driver/experimental/PujjoSofFirmwarePolicy.c'),
                    str(root/'tests/test_pujjo_sof_fw_policy.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: firmware candidate policy host tests')
