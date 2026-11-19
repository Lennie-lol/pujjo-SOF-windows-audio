# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param([Parameter(Mandatory)] [string]$Path)
$ErrorActionPreference = 'Stop'
if (-not ('PujjoFirmwareTables' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PujjoFirmwareTables {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint GetSystemFirmwareTable(uint provider, uint id,
        [Out] byte[] buffer, uint size);
}
'@
}
# ACPI provider FourCC and little-endian NHLT table identifier.
$provider = [uint32]0x41435049
$table = [uint32]0x544c484e
$size = [PujjoFirmwareTables]::GetSystemFirmwareTable($provider, $table, $null, 0)
if ($size -eq 0) {
    [pscustomobject]@{ NHLT = 'Unavailable'; Win32Error = [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
    return
}
if ($size -lt 37 -or $size -gt 1MB) { throw "NHLT size outside parser bounds ($size bytes)" }
$bytes = [byte[]]::new($size)
$actual = [PujjoFirmwareTables]::GetSystemFirmwareTable($provider, $table, $bytes, $size)
if ($actual -ne $size -or [BitConverter]::ToUInt32($bytes, 0) -ne $table) {
    throw 'NHLT changed or returned the wrong signature during acquisition'
}
$length = [BitConverter]::ToUInt32($bytes, 4)
if ($length -lt 37 -or $length -gt $size) { throw 'Invalid NHLT declared length' }
$sum = 0
for ($i = 0; $i -lt $length; $i++) { $sum = ($sum + $bytes[$i]) -band 255 }
if ($sum -ne 0) { throw 'NHLT checksum failed' }

$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
$file = [IO.File]::Open($output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $file.Write($bytes, 0, $length) } finally { $file.Dispose() }
[pscustomobject]@{
    NHLT = $output
    Bytes = $length
    SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash
}
