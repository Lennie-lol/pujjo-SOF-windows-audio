#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Conservative static constraints for read-only Pujjo inventory script."""
from pathlib import Path
script=(Path(__file__).resolve().parents[1]/'scripts/ReadOnly-PujjoIpc4Evidence.ps1').read_text()
for token in ('Get-CimInstance', 'Get-PnpDevice', 'Get-PnpDeviceProperty',
              'VEN_8086&DEV_54C8', 'VEN_8086&DEV_54EB',
              'RTL5682', 'RTL1019', 'DEVPKEY_Device_LocationPaths'):
    assert token in script, token
for forbidden in ('Set-PnpDevice', 'Disable-PnpDevice', 'Enable-PnpDevice',
                  'Add-WindowsDriver', 'pnputil', 'bcdedit', 'Invoke-WebRequest',
                  'Invoke-RestMethod', 'Set-Content', 'New-Item', 'Set-Item',
                  'Start-Service', 'Stop-Service', 'Set-ExecutionPolicy',
                  'IOCTL_GPIO', 'IOCTL_SPB', 'WdfIoTarget', 'Write-EventLog'):
    assert forbidden.lower() not in script.lower(), forbidden
print('PASS: read-only Pujjo device identity and topology report script')
