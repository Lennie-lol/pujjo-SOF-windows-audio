#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Pure host integration of IPC4 readiness and SSP1 bring-up; no WDK or hardware."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
exp = root / 'src/driver/experimental'
cc = os.environ.get('CC') or next((x for x in ('clang', 'gcc', 'cc') if shutil.which(x)), None)
if not cc:
    raise SystemExit('FAIL: C compiler required for boot/speaker integration tests')
sources = [exp / n for n in ('PujjoIpc4BootModel.c', 'PujjoBringupCoordinator.c',
           'PujjoAudioInit.c', 'PujjoRt1019pAmp.c', 'PujjoSofFirmwarePolicy.c')]
sources.append(root / 'src/driver/PujjoIpc4Protocol.c')
sources.append(root / 'tests/test_pujjo_boot_to_speaker.c')
with tempfile.TemporaryDirectory(prefix='pj_boot_speaker_') as tmp:
    out = Path(tmp) / ('speaker.exe' if os.name == 'nt' else 'speaker')
    for mode, flags in (
        ('locked', []),
        ('host-mock', ['-DPJ_IPC4_HOST_TEST', '-DPJ_BRINGUP_TEST_CALLBACKS',
                       '-DPJ_BRINGUP_HOST_TEST']),
    ):
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wconversion', '-Wsign-conversion', '-pedantic',
                        *flags, *map(str, sources), '-o', str(out)], check=True, timeout=40)
        subprocess.run([str(out)], check=True, timeout=15)
        print('PASS:', mode, 'IPC4-to-speaker simulated lifecycle')
