#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fail if source-only mock coordinator is wired into Windows driver targets."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
name = 'PujjoBringupCoordinator'
source = (ROOT / 'src' / 'driver' / 'experimental' / f'{name}.c').read_text()
for flag in ('PJ_BRINGUP_TEST_CALLBACKS', 'PJ_BRINGUP_HOST_TEST'):
    assert flag in source, f'Missing explicit guard for {flag}'
for project in ROOT.glob('*.vcxproj'):
    data = project.read_text(encoding='utf-8')
    for token in ('PJ_BRINGUP_TEST_CALLBACKS', 'PJ_BRINGUP_HOST_TEST', f'{name}.c'):
        assert token not in data, f'Unsafe host mock inclusion in {project.name}: {token}'
workflow = ROOT / '.github' / 'workflows' / 'compile-windows.yml'
for token in ('PJ_BRINGUP_TEST_CALLBACKS', 'PJ_BRINGUP_HOST_TEST'):
    assert token not in workflow.read_text(encoding='utf-8')
# Ensure boot and physical activation cannot be enabled by this source revision.
loader = (ROOT / 'src' / 'driver' / 'Loader.c').read_text()
dev = (ROOT / 'src' / 'driver' / 'Device.c').read_text()
assert 'PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT' in loader
assert 'PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT' in dev
assert 'PJ1019_TEST_ONLY_CALLBACKS' not in '\n'.join(p.read_text(encoding='utf-8') for p in ROOT.glob('*.vcxproj'))
print('PASS: mock-only coordinator not linked to WDK targets; existing firmware boot lock present')
