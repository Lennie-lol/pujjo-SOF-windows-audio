# SPDX-License-Identifier: BSD-3-Clause
<#
  Read-only Pujjo/Pujjo1e device and IPC4 development evidence.
  Never installs or starts a driver, reads/writes firmware registers,
  disables Windows protections, changes services, or sends network data.
  Does not collect usernames, serial numbers, IP addresses or MAC addresses.
  Usage (on physical Pujjo Chromebook running Windows, not on a VM):
      .\scripts\ReadOnly-PujjoIpc4Evidence.ps1
  Review output before sharing; it includes PnP hardware identifiers.
#>
[CmdletBinding()]
param()
$ErrorActionPreference='Continue'
Write-Output 'Pujjo IPC4 evidence: READ-ONLY, no device activation'
try {
    $board=Get-CimInstance Win32_BaseBoard -ErrorAction Stop
    $maker=[string]$board.Manufacturer
    $name=[string]$board.Product
    Write-Output ('Board: {0} / {1}' -f $maker,$name)
    if ($maker -ne 'Google' -or $name -notin @('Pujjo','Pujjo1e')) {
        Write-Warning 'Board identity unverified; nothing in this report authorizes a driver installation.'
    }
} catch { Write-Warning ('Board inventory unavailable: ' + $_.Exception.Message) }
try {
    $os=Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Write-Output ('OS build: {0} ({1})' -f $os.Version,$os.BuildNumber)
} catch { Write-Warning 'OS version unavailable' }
$tokens=@('VEN_8086&DEV_54C8','VEN_8086&DEV_54EB','ACPI\RTL5682','ACPI\RTL1019')
$keys=@(
    'DEVPKEY_Device_HardwareIds',
    'DEVPKEY_Device_Parent',
    'DEVPKEY_Device_LocationPaths',
    'DEVPKEY_Device_Service',
    'DEVPKEY_Device_ProblemCode'
)
try {
    $found=@(Get-PnpDevice -ErrorAction Stop | Where-Object {
        $id=[string]$_.InstanceId
        foreach($token in $tokens) { if ($id.Contains($token)) { return $true } }
        return $false
    })
    if ($found.Count -eq 0) { Write-Warning 'No matching audio/PCH I2C devices found' }
    foreach($dev in $found) {
        Write-Output ('--- Device: {0}; State: {1}; ID: {2}' -f $dev.FriendlyName,$dev.Status,$dev.InstanceId)
        foreach($key in $keys) {
            try {
                $prop=Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $key -ErrorAction Stop
                $items=@($prop.Data | ForEach-Object { [string]$_ })
                Write-Output ('{0}: {1}' -f $key,($items -join ' | '))
            } catch { Write-Output ('{0}: unavailable' -f $key) }
        }
    }
} catch { Write-Warning ('PnP inventory unavailable: ' + $_.Exception.Message) }
Write-Output 'Note: PnP IDs and location paths cannot prove translated _CRS I2C/GPIO connection IDs.'
Write-Output 'Note: This does not authenticate Intel SOF firmware or determine IPC3 vs IPC4.'
Write-Output 'DONE: read-only observation. No hardware tests, driver installation or audio activation performed.'
