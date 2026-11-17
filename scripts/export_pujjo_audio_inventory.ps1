# SPDX-License-Identifier: BSD-3-Clause
# Read-only PnP capture. Returns structured JSON; no driver install or hardware writes.
param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
function Find-Pnp([string]$Pattern) {
    $matches = @(Get-PnpDevice | Where-Object { $_.InstanceId -like $Pattern })
    foreach ($dev in $matches) {
        $properties = @{}
        foreach ($p in @(Get-PnpDeviceProperty -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue)) {
            if ($p.KeyName -match 'Device_(HardwareIds|ProblemCode|Parent|LocationPaths)') {
                $properties[$p.KeyName -replace '^DEVPKEY_Device_', ''] = @($p.Data)
            }
        }
        [PSCustomObject]@{
            InstanceId = $dev.InstanceId
            Status = [string]$dev.Status
            Properties = $properties
        }
    }
}
$board = Get-CimInstance Win32_BaseBoard
$bios = Get-CimInstance Win32_BIOS
$inventory = [PSCustomObject]@{
    SchemaVersion = 1
    Board = [PSCustomObject]@{
        Manufacturer = $board.Manufacturer
        Product = $board.Product
        Revision = $board.Version
        Firmware = $bios.SMBIOSBIOSVersion
    }
    Intel = @(Find-Pnp 'PCI\VEN_8086&DEV_54C8*')
    Rt5682 = @(Find-Pnp 'ACPI\RTL5682*')
    Rt1019 = @(Find-Pnp 'ACPI\RTL1019*')
    RequiresResourceValidation = $true
    RealHardwareWritesEnabled = $false
}
$json = $inventory | ConvertTo-Json -Depth 8
if ($OutputPath) {
    $json | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    Write-Output "Inventory saved to: $OutputPath"
} else { Write-Output $json }
# IMPORTANT: PnP parents and ACPI paths do not provide I2C addresses or GPIO resource descriptors.
