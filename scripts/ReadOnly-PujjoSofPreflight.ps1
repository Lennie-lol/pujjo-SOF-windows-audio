# SPDX-License-Identifier: BSD-3-Clause
<#
 Read-only Pujjo SOF diagnostic: no administrator rights, WDK, reboot,
 registry edits, signing, driver installation, GPIO or MMIO writes.
 Does not collect hardware serial numbers or send data over the network.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'
Write-Host 'Pujjo SOF read-only preflight (no driver is installed or loaded)' -ForegroundColor Cyan
try {
    $bb = Get-CimInstance Win32_BaseBoard -ErrorAction Stop
    Write-Host ('Baseboard manufacturer: {0}; product: {1}' -f $bb.Manufacturer,$bb.Product)
    if ($bb.Manufacturer -ne 'Google' -or $bb.Product -notin @('Pujjo','Pujjo1e')) {
        Write-Warning 'Baseboard name does not match expected Pujjo/Pujjo1e. Stop development driver testing.'
    }
} catch { Write-Warning ('Baseboard read not available: ' + $_.Exception.Message) }
$patterns = @('VEN_8086&DEV_54C8','ACPI\RTL5682','ACPI\RTL1019','RTL5682','RTL1019')
try {
    $devices = Get-PnpDevice -ErrorAction Stop | Where-Object {
        $instance = [string]$_.InstanceId
        foreach ($token in $patterns) { if ($instance.Contains($token)) { return $true } }
        return $false
    }
    if (@($devices).Count -eq 0) { Write-Warning 'No matching PnP device IDs found.' }
    foreach ($dev in $devices) {
        Write-Host ('Device: {0}; Status: {1}; ID: {2}' -f $dev.FriendlyName,$dev.Status,$dev.InstanceId)
    }
} catch { Write-Warning ('PnP inventory not available: ' + $_.Exception.Message) }
$paths = @(
    "$env:windir\System32\drivers\intel\sof\sof-adl-n.ri",
    "$env:windir\System32\drivers\intel\sof-ipc4\adl-n\sof-adl-n.ri"
)
foreach ($path in $paths) {
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $file = Get-Item -LiteralPath $path
        Write-Host ('Firmware candidate present: {0}; size: {1} bytes' -f $path,$file.Length)
        Write-Warning 'Presence does NOT establish firmware compatibility, signature trust, or working DSP.'
    } else { Write-Host ('Firmware candidate not found at inspected path: {0}' -f $path) }
}
Write-Host 'IPC3 versus IPC4 cannot be determined by file presence or the PCI ID alone.'
Write-Host 'PASS: read-only inventory completed; audio functionality NOT tested.'
