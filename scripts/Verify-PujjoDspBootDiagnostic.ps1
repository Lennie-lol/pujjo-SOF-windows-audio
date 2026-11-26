# SPDX-License-Identifier: BSD-3-Clause
<#
Read-only preflight for the UNSIGNED Pujjo rev3 DSP boot-only CI submission.
Optionally verifies pinned SOF firmware, exact board and authorized signed package.
Never installs, arms, signs, loads or modifies a driver or boot configuration.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SubmissionDirectory,
    [string]$ExpectedRevision,
    [string]$FirmwarePath,
    [string]$SignedPackageDirectory,
    [string]$SignToolPath,
    [switch]$InspectLocalBoard
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Require([bool]$OK, [string]$Why) {
    if (-not $OK) { throw "DSP boot preflight FAILED: $Why" }
}
function Digest([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}
$root = (Resolve-Path -LiteralPath $SubmissionDirectory -ErrorAction Stop).ProviderPath
$package = Join-Path $root 'submission'
Require (Test-Path -LiteralPath $package -PathType Container) 'submission/ directory missing'
$manifest = Join-Path $root 'sha256.json'
$warning = Join-Path $root 'UNSIGNED-SUBMISSION-ONLY.txt'
Require (Test-Path -LiteralPath $manifest -PathType Leaf) 'Missing sha256.json'
Require (Test-Path -LiteralPath $warning -PathType Leaf) 'Missing unsigned warning'
$notice = Get-Content -LiteralPath $warning -Raw
Require ($notice -match '(?i)unsigned') 'Submission has no unsigned warning'
if ($ExpectedRevision) {
    Require ($ExpectedRevision -match '^[0-9a-fA-F]{40}$') 'ExpectedRevision must be a full commit SHA'
    Require ($notice.Contains("Source revision $ExpectedRevision")) 'Source revision mismatch'
}
$names = @('sof_dsp.inf', 'sof_dsp.sys', 'sof_dsp.cat')
$files = @(Get-ChildItem -LiteralPath $package -File -Force | Select-Object -ExpandProperty Name)
Require ($files.Count -eq 3) 'Unexpected number of submission files'
# In Windows PowerShell 5.1, ConvertFrom-Json may emit a JSON array as one
# pipeline object. A positional @(... | ConvertFrom-Json) then has Count=1.
# Explicitly enumerate the parsed array, independent of shell version.
$parsed = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $manifest -Raw)
$records = New-Object 'System.Collections.Generic.List[object]'
foreach ($entry in $parsed) { [void]$records.Add($entry) }
Require ($records.Count -eq 3) 'Manifest must have exactly three entries'
foreach ($name in $names) {
    Require ($files -contains $name) "Missing $name"
    $match = @($records | Where-Object { $_.File -eq $name })
    Require ($match.Count -eq 1) "Duplicate or missing hash for $name"
    Require ($match[0].Algorithm -eq 'SHA256' -and [string]$match[0].Hash -match '^[0-9a-fA-F]{64}$') "Malformed hash for $name"
    Require ((Digest (Join-Path $package $name)) -eq [string]$match[0].Hash) "Hash mismatch for $name"
}
$infText = Get-Content -LiteralPath (Join-Path $package 'sof_dsp.inf') -Raw
$versionLines = @($infText -split '\r?\n' | Where-Object { $_ -match '^\s*DriverVer\s*=' })
Require ($versionLines.Count -eq 1) 'Expected exactly one DriverVer line'
Require ($versionLines[0].Trim() -eq 'DriverVer=11/26/2026,0.0.6.0') 'Wrong diagnostic package version; expected 0.0.6.0'
Require ($notice.Contains('Package version 0.0.6.0;')) 'Missing diagnostic package version in notice'
Require ($notice.Contains('DMA and firmware execution locked')) 'Missing DMA lifetime hold notice'
Require ($infText.Contains('PCI\VEN_8086&DEV_54C8&SUBSYS_00000000')) 'Wrong DSP PCI ID'
Require ($infText.Contains('AddService = sof_dsp')) 'Wrong or missing driver service'
foreach ($setting in @('SafeBootPolicy','BootSafetyToken','ArmedTesting','EnablePortCls','EnableS0Idle','EnableUsbOffload')) {
    Require ($infText.Contains("HKR,Parameters,$setting,0x00010001,0")) "Unsafe INF default: $setting"
}
Require (-not [regex]::IsMatch($infText, '(?im)^\s*AddInterface\s*=')) 'Unexpected audio interface'
Require ((Get-AuthenticodeSignature -LiteralPath (Join-Path $package 'sof_dsp.sys')).Status -eq 'NotSigned') 'Expected unsigned CI SYS'
Write-Output 'PASS: submission files, hashes, target identity and forced-safe defaults.'

if ($FirmwarePath) {
    Require (Test-Path -LiteralPath $FirmwarePath -PathType Leaf) 'Firmware file missing'
    Require ((Get-Item -LiteralPath $FirmwarePath).Length -eq 707036) 'Unexpected ADL-N IPC4 firmware length'
    Require ((Digest $FirmwarePath) -eq 'BC83CFFA1BB5A4A8DA24BBC7BB7040BD744F1E0C63E3D16151D1769AC8AE8214') 'ADL-N firmware digest mismatch'
    Write-Output 'PASS: exact pinned SOF IPC4 firmware digest. ROM authentication remains untested.'
} else { Write-Warning 'Firmware not supplied; DSP boot prerequisite remains incomplete.' }

if ($InspectLocalBoard) {
    $boards = @(Get-CimInstance Win32_BaseBoard -ErrorAction Stop)
    Require ($boards.Count -eq 1) 'Baseboard not uniquely identified'
    Require ($boards[0].Manufacturer -eq 'Google' -and $boards[0].Product -eq 'Pujjo') 'Not Google Pujjo'
    Require ([string]$boards[0].Version -eq 'rev3') 'Pujjo rev3 not confirmed'
    $dsp = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
        ([string]$_.InstanceId) -match '^PCI\\VEN_8086&DEV_54C8&SUBSYS_00000000(?:&|\\)'
    })
    Require ($dsp.Count -eq 1) 'No unique 8086:54C8 PnP device'
    Write-Output 'PASS: read-only physical Google Pujjo rev3 identity.'
}

if ($SignedPackageDirectory) {
    $signed = (Resolve-Path -LiteralPath $SignedPackageDirectory -ErrorAction Stop).ProviderPath
    foreach ($name in $names) {
        Require (Test-Path -LiteralPath (Join-Path $signed $name) -PathType Leaf) "Signed package missing $name"
    }
    foreach ($name in @('sof_dsp.inf', 'sof_dsp.sys')) {
        Require ((Digest (Join-Path $signed $name)) -eq (Digest (Join-Path $package $name))) "Signed package modified $name"
    }
    if (-not $SignToolPath) {
        $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
        if ($null -ne $cmd) { $SignToolPath = $cmd.Source }
    }
    Require ($SignToolPath -and (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) 'Supply WDK SignTool.exe via -SignToolPath'
    $cat = Join-Path $signed 'sof_dsp.cat'
    Require ((Get-AuthenticodeSignature -LiteralPath $cat).Status -eq 'Valid') 'Catalog signature is not trusted'
    & $SignToolPath verify /kp /v $cat
    Require ($LASTEXITCODE -eq 0) 'Catalog fails Windows kernel-policy verification'
    foreach ($name in @('sof_dsp.inf', 'sof_dsp.sys')) {
        & $SignToolPath verify /kp /v /c $cat (Join-Path $signed $name)
        Require ($LASTEXITCODE -eq 0) "Catalog membership invalid for $name"
    }
    Write-Output 'PASS: signed INF/SYS unchanged; kernel-policy catalog and membership verified.'
} else { Write-Warning 'No authorized signed package supplied. Do not install unsigned CI output.' }
Write-Output 'STOP: Read-only verification only. No driver, firmware boot or audio hardware was activated.'
