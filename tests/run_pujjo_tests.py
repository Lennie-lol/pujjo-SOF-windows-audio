#!/usr/bin/env python3
"""Portable, hardware-free tests for the Pujjo-specific source changes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
src = root / 'src' / 'driver'

with tempfile.TemporaryDirectory(prefix='pujjo_test_') as tmp:
    exe = Path(tmp) / 'boardid_test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(src / 'PujjoBoardId.c'),
                    str(root / 'tests' / 'test_pujjo_boardid.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

inf = (src / 'sof_dsp.inx').read_text()
assert 'PCI\\VEN_8086&DEV_54C8&SUBSYS_00000000' in inf
assert 'PCI\\VEN_8086&DEV_51C8' not in inf
assert 'PCI\\VEN_1022&DEV_' not in inf

device = (src / 'Device.c').read_text()
assert 'PujjoBoardIdentityMatches()' in device
prepare = device.split("SofEvtDevicePrepareHardware(", 1)[1]
assert prepare.index('PujjoBoardIdentityMatches()') < prepare.index('WdfFdoQueryForInterface') < prepare.index('SofLogResetCapability(Device)')
assert 'busInterface.SetBusData(' not in device
assert 'PCI Command updated:' not in device
assert 'ctx->VendorId != 0x8086 || ctx->DeviceId != 0x54C8' in device
assert 'SofIpc4CreateSspPipeline(ctx, 4, 4, 2' not in device
wave = (src / 'WaveRt.c').read_text()
assert '[PUJJO] SSP audio blocked' in wave
assert 'SofIpc4CreateSspPipeline(Ctx, stream->PipelineId, stream->StreamTag, 2,' not in wave
print('PASS: Pujjo source safety and matching checks')
