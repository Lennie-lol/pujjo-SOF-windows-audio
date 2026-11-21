#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Static safety and resource-gate check; no hardware or compiler required."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
probe=(root/'src/driver/peripheral/PujjoPeripheralKmdf.c').read_text()
win=(root/'src/driver/peripheral/PujjoRt5682sSpbWindows.c').read_text()
core=(root/'src/driver/experimental/PujjoRt5682sCodec.c').read_text()
project=(root/'pujjo_rt5682_probe.vcxproj').read_text()
assert 'PujjoBoardIdentityMatches()' in probe
assert 'PjHardwareIdMatches(Device)' in probe
assert 'pj_validate_resources' in probe
assert '#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)' in probe
assert 'Pj5682sSpbOpen(' in probe, 'Optional read-only PnP staging should be present'
assert 'Pj5682sSpbBindCallbacks(' in probe
assert 'PJ5682S_ENABLE_READONLY_PROBE' not in project, 'Read-only probing must default OFF'
assert 'PJ5682S_TEST_ONLY_WRITES' in core
assert 'return PJ5682S_WRITE_LOCKED;' in core
assert 'PJ5682S_EXPECTED_DEVICE_ID' in core
assert 'WdfIoTargetSendIoctlSynchronously' in win
assert 'IOCTL_SPB_EXECUTE_SEQUENCE' in win
assert 'RESOURCE_HUB_CREATE_PATH_FROM_ID' in win
assert 'Pj5682sSpbOpen' in win and 'Pj5682sSpbClose' in win
assert 'PujjoRt5682sCodec.c' in project
assert 'PujjoRt5682sSpbWindows.c' in project
assert 'pj5682s_cold_invalidate(&state->Cold);' in win, 'Power/close must invalidate epoch'
assert 'pj5682s_cold_is_ready(&state->Cold)' in win, 'Live routes must be gated on cold READY'
assert 'state->Control.faulted = 1;' in win, 'Stale rollback across D3 must latch fault'
assert 'PJ5682S_ENABLE_LAB_COLD_INIT' not in project, 'Lab writes must be OFF in WDK'
assert '#ifdef PJ5682S_ENABLE_LAB_COLD_INIT' in win, 'Lab bridge must require opt-in'
assert 'state->ColdAttempted = TRUE' in win, 'Only one cold attempt per epoch'
assert 'state->ColdTransactionActive = FALSE;' in win, 'Clear private I2C permission'
assert 'status = StopControlLocked(state);\n        }' not in win, 'No cross-epoch rollback attempt'
assert not list(root.rglob('*.inf')) and not list(root.rglob('*.cat'))
print('PASS: RT5682S source safety gates and optional Windows SPB connection')
