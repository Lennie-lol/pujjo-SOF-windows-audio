"""Fail-closed static checks (not a replacement for WDK SDV / HLK)."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
p = (root/'src/driver/peripheral/PujjoPeripheralKmdf.c').read_text()
pol = (root/'src/driver/peripheral/PujjoResourcePolicy.c').read_text()
gui = (root/'Build_Pujjo.pyw').read_text().lower()
assert 'PujjoBoardIdentityMatches()' in p
assert 'PjHardwareIdMatches(Device)' in p
assert 'WdfDeviceQueryProperty' in p
assert 'DevicePropertyHardwareID' in p
assert 'PjEvtPrepareHardware' in p and 'PjEvtReleaseHardware' in p
assert 'WdfCmResourceListGetDescriptor' in p
assert 'CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C' in p
assert 'CM_RESOURCE_CONNECTION_TYPE_GPIO_IO' in p
assert 'PJ_RES_UNKNOWN_CONNECTION' in p
assert 'STATUS_DEVICE_CONFIGURATION_ERROR' in p
assert 'PJ_MAX_RESOURCES' in p
assert 'PJ_POLICY_DUPLICATE_ID' in pol
assert 'PJ_POLICY_EXCESSIVE_RESOURCES' in pol
for banned in ('WdfIoTargetOpen', 'IOCTL_GPIO_WRITE_PINS',
               'IOCTL_SPB_EXECUTE_SEQUENCE', 'WdfIoTargetSendWriteSynchronously',
               'WRITE_REGISTER_', 'READ_REGISTER_'):
    assert banned not in p, f'Unexpected hardware access: {banned}'
for banned in ('bcdedit', 'testsigning', 'pnputil', 'certutil', 'disable-integrity',
               'executionpolicy bypass', 'install_driver', 'sign_driver', 'inf2cat'):
    assert banned not in gui, f'Unsafe build step: {banned}'
assert 'msbuild' in gui and '/p:platform=x64' in gui
assert 'test_kmdf_security.py' in gui
assert not list(root.rglob('*.inf')), 'Do not ship installable INFs in the prototype'
assert not list(root.glob('*.bat')), 'No .bat launcher in this package'
print('PASS: build safety and KMDF source checks')
