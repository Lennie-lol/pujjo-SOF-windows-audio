#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Static assertions only. Do NOT imply the kernel driver compiles or works."""
from pathlib import Path
p=Path(__file__).resolve().parents[1]
loader=(p/'src/driver/Loader.c').read_text()
device=(p/'src/driver/Device.c').read_text()
policy=(p/'src/driver/experimental/PujjoSofFirmwarePolicy.c').read_text()
script=(p/'scripts/ReadOnly-PujjoSofPreflight.ps1').read_text()
proj=(p/'sof_dsp.vcxproj').read_text()
assert '#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)' in loader
assert '#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)' in device
assert 'return STATUS_NOT_SUPPORTED' in loader and 'return STATUS_NOT_SUPPORTED' in device
assert 'Ctx->VendorId != 0x8086 || Ctx->DeviceId != 0x54C8' in loader
assert 'PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED' in loader
assert 'sof-adl-n.ri' in loader and 'sof-ipc4' in loader
assert 'sof-tgl.ri' not in loader and 'defaultPath' not in loader
assert 'BCryptHash(' in loader and 'STATUS_INVALID_IMAGE_HASH' in loader
assert 'sof-tgl.ri' not in policy
assert 'sof-ipc4/adl-n/intel-signed/sof-adl-n.ri' in policy
assert 'sof/intel-signed/sof-adl-n.ri' in policy
assert 'PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT' not in proj
assert 'PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED' not in proj
for phrase in ('pnputil','bcdedit','New-Item','Set-Item','Set-Content','Set-ExecutionPolicy','Invoke-WebRequest','WdfIoTarget'):
    assert phrase.lower() not in script.lower(), phrase
assert not list(p.rglob('*.sys')) and not list(p.rglob('*.inf'))
print('PASS: firmware default boot block; ADL-N selection; read-only Chromebook preflight')
