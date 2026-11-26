# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BinaryDirectory,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-f]{40}$')] [string]$SourceRevision,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$infVerif = @(
    (Join-Path $kitRoot "Tools\$KitVersion\x64\InfVerif.exe")
    (Join-Path $kitRoot "bin\$KitVersion\x64\InfVerif.exe")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$inf2Cat = Join-Path $kitRoot "bin\$KitVersion\x86\Inf2Cat.exe"
if (!$infVerif -or !(Test-Path -LiteralPath $inf2Cat -PathType Leaf)) { throw 'Missing WDK validation tools' }
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output must be a new directory' }
$package = New-Item -ItemType Directory -Path (Join-Path $output 'submission') -Force
$inf = Join-Path $package.FullName 'sof_dsp.inf'
$sys = Join-Path $package.FullName 'sof_dsp.sys'
$cat = Join-Path $package.FullName 'sof_dsp.cat'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\packaging\diagnostics\pujjo_dsp_boot_only.inx') -Destination $inf
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'sof_dsp.sys') -Destination $sys
if ((Get-AuthenticodeSignature $sys).Status -ne 'NotSigned') { throw 'Expected unsigned diagnostic' }
& $infVerif /w /v $inf
if ($LASTEXITCODE -ne 0) { throw 'DSP diagnostic InfVerif failed' }
& $inf2Cat "/driver:$($package.FullName)" '/os:10_GE_X64'
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $cat -PathType Leaf)) { throw 'DSP diagnostic Inf2Cat failed' }
Get-FileHash -Algorithm SHA256 -LiteralPath $inf,$sys,$cat |
    Select-Object Algorithm,Hash,@{Name='File';Expression={Split-Path -Leaf $_.Path}} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'sha256.json')
Set-Content -LiteralPath (Join-Path $output 'UNSIGNED-SUBMISSION-ONLY.txt') -Value @(
    "Source revision $SourceRevision"
    'Package version 0.0.8.0; distinct from previous signed packages.'
    'UNSIGNED DSP diagnostic submission input; forced-safe defaults; one-shot read-only resource probe only; DMA and firmware execution locked; not hardware verified, signed or authorized for installation.'
    'Microsoft kernel-policy signing and catalog membership verification required.'
    'No firmware is bundled. Required pinned firmware: 707036 bytes; SHA256 bc83cffa1bb5a4a8da24bbc7bb7040bd744f1e0c63e3d16151d1769ac8ae8214.'
    'Embedded DMIC bytes are read-only evidence; no replacement NHLT activated, PortCls package, codec or amplifier control. No installer.'
)
