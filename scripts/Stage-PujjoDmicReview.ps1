# SPDX-License-Identifier: BSD-3-Clause
# Stage an UNSIGNED DMIC-only child package. NEVER installs, signs or arms hardware.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BinaryDirectory,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$SourceRevision,
    [string]$KitVersion='10.0.26100.0'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$verif=@(
  (Join-Path $root "Tools\$KitVersion\x64\InfVerif.exe")
  (Join-Path $root "bin\$KitVersion\x64\InfVerif.exe")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$inf2cat=Join-Path $root "bin\$KitVersion\x86\Inf2Cat.exe"
if (!$verif -or !(Test-Path -LiteralPath $inf2cat -PathType Leaf)) {
    throw 'Missing WDK InfVerif or Inf2Cat'
}
$out=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw 'Output must be a new directory' }
$pkg=New-Item -ItemType Directory -Path (Join-Path $out 'submission') -Force
$inf=Join-Path $pkg.FullName 'pujjo_audio.inf'
$sys=Join-Path $pkg.FullName 'pujjo_audio.sys'
$cat=Join-Path $pkg.FullName 'pujjo_audio.cat'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\packaging\review\pujjo_dmic_only.inx') -Destination $inf
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'pujjo_audio.sys') -Destination $sys
if ((Get-AuthenticodeSignature -LiteralPath $sys).Status -ne 'NotSigned') {
    throw 'Expected unsigned PortCls development binary'
}
& $verif /w /v $inf
if ($LASTEXITCODE -ne 0) { throw 'DMIC review InfVerif failed' }
& $inf2cat "/driver:$($pkg.FullName)" '/os:10_GE_X64'
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $cat -PathType Leaf)) {
    throw 'DMIC review Inf2Cat failed'
}
Get-FileHash -Algorithm SHA256 -LiteralPath $inf,$sys,$cat |
    Select-Object Algorithm,Hash,@{Name='File';Expression={Split-Path -Leaf $_.Path}} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'sha256.json')
Set-Content -LiteralPath (Join-Path $out 'UNSIGNED-DMIC-REVIEW-ONLY.txt') -Value @(
  "Source revision $SourceRevision"
  'UNSIGNED DMIC-only PortCls child review artifact; NOT approved for installation.'
  'No DSP firmware, PCI ROM/DMA activation, codec or amplifier control is included.'
  'Only a genuinely running DSP with validated FW_READY, FW_CONFIG, and approved DMIC NHLT can publish this child.'
  'The known-good v0.0.2.0 forced-safe DSP stub will not publish this microphone.'
  'Physical tests, catalog signing under kernel policy, and rehearsed recovery remain mandatory.'
)
