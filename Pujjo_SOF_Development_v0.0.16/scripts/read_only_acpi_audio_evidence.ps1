# Pujjo ACPI/PnP inventory; read-only. Does not install drivers or read/write registers.
# SPDX-License-Identifier: BSD-3-Clause
$ErrorActionPreference = 'Stop'
$board = Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer, Product, Version
$bios = Get-CimInstance Win32_BIOS | Select-Object Manufacturer, SMBIOSBIOSVersion
Write-Host "=== Board and firmware ==="
$board | Format-List
$bios | Format-List
$patterns = @('PCI\VEN_8086&DEV_54C8*', 'ACPI\RTL5682*', 'ACPI\RTL1019*')
Write-Host "=== PnP evidence (NO Windows SPB/GPIO resource acquisition) ==="
foreach ($pattern in $patterns) {
    $devices = @(Get-PnpDevice | Where-Object { $_.InstanceId -like $pattern })
    if ($devices.Count -eq 0) { Write-Host "NOT FOUND: $pattern"; continue }
    foreach ($device in $devices) {
        Write-Host ("`nDevice: {0}" -f $device.InstanceId)
        $device | Format-List Status, Class, FriendlyName, InstanceId
        $props = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -ErrorAction SilentlyContinue |
            Where-Object { $_.KeyName -match 'HardwareIds|CompatibleIds|Parent|LocationPaths|Service|ProblemCode|BusReportedDeviceDesc|Driver' } |
            Select-Object KeyName, Data)
        $props | Format-List
    }
}
Write-Warning 'Device enumeration does NOT establish I2C address, SDB GPIO polarity, SSP wiring, or speaker-safe volume.'
Write-Warning 'This script intentionally does not attempt to access ACPI _CRS or control any speaker hardware.'
