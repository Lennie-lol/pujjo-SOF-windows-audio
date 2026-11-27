# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$ParentBinaryDirectory,
    [Parameter(Mandatory)] [string]$ChildBinaryDirectory,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$SourceRevision,
    [string]$KitVersion = '10.0.26100.0'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$verif = @(
    (Join-Path $kit "Tools\$KitVersion\x64\InfVerif.exe")
    (Join-Path $kit "bin\$KitVersion\x64\InfVerif.exe")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$inf2cat = Join-Path $kit "bin\$KitVersion\x86\Inf2Cat.exe"
if (!$verif -or !(Test-Path -LiteralPath $inf2cat -PathType Leaf)) { throw 'Missing WDK validation tools' }
$out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw 'Output must be new' }
$manifest = @()
foreach ($spec in @(
    @{ Dir='parent'; Base='sof_dsp'; Template='pujjo_dsp_dmic_detection.inx'; Binary=(Join-Path $ParentBinaryDirectory 'sof_dsp.sys') },
    @{ Dir='child'; Base='pujjo_audio_detection'; Template='pujjo_dmic_detection_child.inx'; Binary=(Join-Path $ChildBinaryDirectory 'pujjo_audio.sys') }
)) {
    $dir = (New-Item -ItemType Directory -Path (Join-Path $out $spec.Dir) -Force).FullName
    $inf = Join-Path $dir ($spec.Base + '.inf')
    $sys = Join-Path $dir ($spec.Base + '.sys')
    $cat = Join-Path $dir ($spec.Base + '.cat')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('..\packaging\diagnostics\' + $spec.Template)) -Destination $inf
    Copy-Item -LiteralPath $spec.Binary -Destination $sys
    if ((Get-AuthenticodeSignature -LiteralPath $sys).Status -ne 'NotSigned') { throw 'Expected unsigned binary' }
    & $verif /w /v $inf
    if ($LASTEXITCODE -ne 0) { throw "InfVerif failed: $inf" }
    & $inf2cat "/driver:$dir" '/os:10_GE_X64'
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $cat -PathType Leaf)) { throw "Inf2Cat failed: $inf" }
    $manifest += @(Get-FileHash -Algorithm SHA256 -LiteralPath $inf,$sys,$cat | ForEach-Object {
        @{ File=($spec.Dir + '/' + (Split-Path -Leaf $_.Path)); Hash=$_.Hash }
    })
}
@{ Version='0.0.8.1'; Revision=$SourceRevision; Files=$manifest } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'sha256.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Verify-PujjoDmicDetection.ps1') -Destination $out
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\docs\PUJJO_MIC_DETECTION_V8_1.md') -Destination $out
@(
    "Source revision $SourceRevision"
    'Package version 0.0.8.1: unsigned, one-shot microphone enumeration test only.'
    'Catalogs must be test-signed and independently verified before installation. No firmware, DMA, analog controls, or recording.'
    'Physical Windows PnP enumeration not yet verified. Keep recovery packages and tested WinRE.'
) | Set-Content -LiteralPath (Join-Path $out 'UNSIGNED-DETECTION-ONLY.txt')
