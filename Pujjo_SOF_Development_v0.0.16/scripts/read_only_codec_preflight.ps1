# Pujjo RT5682S / RTL1019 amplifier preflight -- READ ONLY.
# Retrieves Windows PnP/SMBIOS information; does not load/modify drivers,
# flip GPIO, talk to I2C, or run unverified kernel code.
$ErrorActionPreference = 'Stop'
$board = Get-CimInstance Win32_BaseBoard
$bios = Get-CimInstance Win32_BIOS
$expected = @(
    @{Role='Intel SOF controller'; Pattern='PCI\\VEN_8086&DEV_54C8*'},
    @{Role='RT5682S codec'; Pattern='ACPI\\RTL5682*'},
    @{Role='RTL1019 amplifier'; Pattern='ACPI\\RTL1019*'}
)
$items = foreach ($target in $expected) {
    $device = Get-PnpDevice | Where-Object { $_.InstanceId -like $target.Pattern } | Select-Object -First 1
    if ($null -eq $device) {
        [pscustomobject]@{Role=$target.Role;Present=$false;Status='Missing';ProblemCode=$null;InstanceId=$null;Location=$null}
        continue
    }
    $code = $null
    $location = $null
    try { $code = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data } catch {}
    try { $location = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_LocationPaths' -ErrorAction Stop).Data -join '; ' } catch {}
    [pscustomobject]@{Role=$target.Role;Present=$true;Status=$device.Status;ProblemCode=$code;InstanceId=$device.InstanceId;Location=$location}
}
$allowed = ($board.Manufacturer -eq 'Google') -and ($board.Product -in @('Pujjo','Pujjo1e'))
[pscustomobject]@{
    Board = $board.Product
    BoardRevision = $board.Version
    Manufacturer = $board.Manufacturer
    AllowedBoard = $allowed
    Firmware = $bios.SMBIOSBIOSVersion
    Devices = @($items)
    SSP1SpeakerPathVerifiedOnWindows = $false
    CodecRegisterProtocolVerifiedOnWindows = $false
    AmplifierGPIOResourcesVerifiedOnWindows = $false
    ExperimentalDriverReady = $false
} | ConvertTo-Json -Depth 5
Write-Warning 'Diagnostic only. No device initialization, kernel driver installation or speaker playback performed.'
