# Read-only Pujjo hardware/firmware preflight. This does NOT install a driver.
$ErrorActionPreference = 'Stop'
$board = Get-CimInstance Win32_BaseBoard
$device = Get-PnpDevice -InstanceId 'PCI\VEN_8086&DEV_54C8*' -ErrorAction SilentlyContinue
$codec = Get-PnpDevice | Where-Object { $_.InstanceId -match '^ACPI\\(RTL5682|RTL1019)\\' }
$bios = Get-CimInstance Win32_BIOS
$allowed = ($board.Manufacturer -eq 'Google') -and ($board.Product -in @('Pujjo', 'Pujjo1e'))
[pscustomobject]@{
    AllowedBoard = $allowed
    Manufacturer = $board.Manufacturer
    Board = $board.Product
    BoardRevision = $board.Version
    IntelAudioControllerPresent = [bool]$device
    RealtekDevices = ($codec.InstanceId -join '; ')
    Firmware = $bios.SMBIOSBIOSVersion
    ExperimentalDriverReady = $false
} | Format-List
if (-not $allowed) { Write-Warning 'Board not on the allowed list.' }
Write-Warning 'This project does not yet implement speaker playback. Do not install this fork.'
