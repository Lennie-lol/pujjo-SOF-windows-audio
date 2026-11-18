#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Static preinstall guardrails. Passing is NOT permission to install."""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
model_h = (root / 'src/driver/experimental/PujjoIpc4BootModel.h').read_text()
model_c = (root / 'src/driver/experimental/PujjoIpc4BootModel.c').read_text()
workflow = (root / '.github/workflows/compile-windows.yml').read_text()
coordinator = (root / 'src/driver/experimental/PujjoBringupCoordinator.c').read_text()
loader = (root / 'src/driver/Loader.c').read_text()
device = (root / 'src/driver/Device.c').read_text()
assert 'HOST-ONLY' in model_h and 'NOT a real firmware loader' in model_h
assert 'PJ_IPC4_HOST_TEST' in model_c and 'must never be enabled in a kernel build' in model_c
assert '#ifndef PJ_IPC4_HOST_TEST' in model_c
assert 'PJ_BOOT_STALE_EVENT' in model_c and 'PJ_BOOT_TIMEOUT' in model_c
assert 'PJ_BOOT_FAULT' in model_c
assert 'PJ_BRINGUP_TEST_CALLBACKS' in coordinator and 'PJ_BRINGUP_HOST_TEST' in coordinator
for text in (model_h, model_c):
    for dangerous in ('WdfIoTarget', 'WRITE_REGISTER_', 'READ_REGISTER_',
                      'IOCTL_GPIO_WRITE_PINS', 'IOCTL_SPB_EXECUTE_SEQUENCE',
                      'MmMapIoSpace', 'SofStartFirmwareBoot('):
        assert dangerous not in text, dangerous
# Every production WDK target remains independent of simulated callbacks.
for project in root.glob('*.vcxproj'):
    tree = ET.parse(project)
    as_text = project.read_text(encoding='utf-8')
    for forbidden in ('PujjoIpc4BootModel', 'PJ_IPC4_HOST_TEST',
                      'PJ_BRINGUP_TEST_CALLBACKS', 'PJ_BRINGUP_HOST_TEST'):
        assert forbidden not in as_text, (project, forbidden)
    assert tree.getroot().tag.endswith('Project')
for code in (loader, device):
    assert '#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)' in code
assert not list(root.rglob('*.inf'))
assert not list(root.rglob('*.cat'))
assert not list(root.rglob('*.sys'))
assert not list(root.rglob('*.exe'))
assert 'branches: [main]' in workflow
assert 'run_offline_suite.py' in workflow
print('PASS: no installable driver package, boot locked, mocks isolated from WDK projects')

from pathlib import Path
driver_dir = Path(__file__).resolve().parents[1] / "src" / "driver"
template = (driver_dir / "sof_dsp.inx").read_text()
for setting in ("ArmedTesting", "EnablePortCls", "EnableUsbOffload", "EnableS0Idle"):
    assert f"HKR,Parameters,{setting},0x00010001,0" in template
assert "SimulateJackState" not in template
assert "SimulateJackState" not in (driver_dir / "HdaVerb.c").read_text()
