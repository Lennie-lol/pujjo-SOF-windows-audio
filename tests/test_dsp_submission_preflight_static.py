#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Static check; full read-only PowerShell verifier also runs on Windows CI."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
script=(root/'scripts/Verify-PujjoDspBootDiagnostic.ps1').read_text()
workflow=(root/'.github/workflows/compile-windows.yml').read_text()
for needle in ('Get-FileHash', 'ConvertFrom-Json', 'sha256.json', 'ExpectedRevision',
               'SUBSYS_00000000', 'SafeBootPolicy', 'BootSafetyToken',
               'Get-AuthenticodeSignature', 'InspectLocalBoard', 'SignedPackageDirectory',
               'SignToolPath', 'verify /kp /v', 'FirmwarePath', '707036',
               'BC83CFFA1BB5A4A8DA24BBC7BB7040BD744F1E0C63E3D16151D1769AC8AE8214'):
    assert needle in script, needle
for forbidden in ('pnputil /add-driver', 'pnputil /delete-driver', 'bcdedit /set',
                  'Set-ItemProperty', 'New-ItemProperty', 'Set-ExecutionPolicy',
                  'New-SelfSignedCertificate', 'Start-Service', 'Enable-PnpDevice'):
    assert forbidden.lower() not in script.lower(), forbidden
assert 'Stage-PujjoDspBootDiagnostic.ps1' in workflow
assert 'Verify-PujjoDspBootDiagnostic.ps1' in workflow
assert workflow.index('Stage-PujjoDspBootDiagnostic.ps1') < workflow.index('Verify-PujjoDspBootDiagnostic.ps1') < workflow.index('Upload unsigned DSP boot-only submission input')
print('PASS: read-only DSP diagnostic artifact verification and Windows CI integration')
