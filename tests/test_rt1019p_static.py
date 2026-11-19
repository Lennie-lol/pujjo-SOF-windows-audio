#!/usr/bin/env python3
"""Static guardrail checks. Passing is not a Windows build or hardware test."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
portable=(root/'src/driver/experimental/PujjoRt1019pAmp.c').read_text()
backend=(root/'src/driver/peripheral/PujjoRt1019pGpioWindows.c').read_text()
probe=(root/'src/driver/peripheral/PujjoPeripheralKmdf.c').read_text()
proj=(root/'pujjo_rt1019_probe.vcxproj').read_text()
assert '#ifndef PJ1019_TEST_ONLY_CALLBACKS' in portable
assert '#error PJ1019_TEST_ONLY_CALLBACKS requires PJ1019_TEST_BUILD' in portable
assert 'IOCTL_GPIO_WRITE_PINS' in backend
assert 'RESOURCE_HUB_CREATE_PATH_FROM_ID' in backend
assert 'WdfIoTargetOpen' in backend
assert 'GENERIC_WRITE' in backend
assert 'Pj1019GpioWrite' not in probe
# The default PnP callbacks cannot open GPIO. The opt-in query-interface
# callback may open it after board, resource, mute and polarity checks.
runtime = probe.split('#ifdef PJ_PERIPHERAL_RUNTIME\nstatic VOID RuntimeLock', 1)[1]
runtime, pnp = runtime.split('#endif\n\nEVT_WDF_DRIVER_DEVICE_ADD', 1)
assert runtime.count('Pj1019GpioOpen(') == 1
assert 'Pj1019GpioOpen(' not in pnp
assert runtime.index('profile->GainLimitApplied != 1u') < runtime.index('Pj1019GpioOpen(')
assert 'defined(PJ_TARGET_AMP) && defined(PJ1019_ENABLE_CONTROL)' in probe

assert 'PJ1019_TEST_ONLY_CALLBACKS' not in proj
assert 'PJ1019_TEST_BUILD' not in proj
assert 'src\\driver\\peripheral\\PujjoRt1019pGpioWindows.c' in proj
assert not list(root.rglob('*.inf')) and not list(root.rglob('*.sys'))
# Must preserve board/ACPI proof and fail-closed resource policy.
assert 'PujjoBoardIdentityMatches()' in probe
assert 'PjHardwareIdMatches(Device)' in probe
assert 'pj_validate_resources(PJ_PROBE_AMP' in probe
for banned in ('gpio_set_value', 'GPIO_PIN_75', 'GPIO_PIN_NUMBER 75'):
    assert banned not in backend
print('PASS: amplifier activation guarded and separate from default PnP; signed/installed artifacts absent')
