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
if (!$infVerif) { throw "InfVerif is missing from WDK $KitVersion" }
if (!(Test-Path -LiteralPath $inf2Cat -PathType Leaf)) { throw "Missing WDK tool $inf2Cat" }

$names = @('pujjo_rt5682_probe', 'pujjo_rt1019_probe')
$templates = Join-Path $PSScriptRoot '..\packaging\diagnostics'
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output must be a new directory' }
New-Item -ItemType Directory -Path $output | Out-Null
Set-Content -LiteralPath (Join-Path $output 'UNSIGNED-SUBMISSION-ONLY.txt') -Value @(
    "Source revision $SourceRevision"
    'Unsigned diagnostic submission input. Catalog signing and kernel-policy verification are required.'
    'The packages contain resource probes only.'
)

foreach ($name in $names) {
    $stage = New-Item -ItemType Directory -Path (Join-Path $output $name)
    $inf = Join-Path $stage.FullName "$name.inf"
    $sys = Join-Path $stage.FullName "$name.sys"
    Copy-Item -LiteralPath (Join-Path $templates "$name.inx") -Destination $inf
    Copy-Item -LiteralPath (Join-Path $BinaryDirectory "$name.sys") -Destination $sys
    & $infVerif /w /v $inf
    if ($LASTEXITCODE -ne 0) { throw "InfVerif failed for $name" }
    & $inf2Cat "/driver:$($stage.FullName)" '/os:10_GE_X64'
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed for $name" }
    $cat = Join-Path $stage.FullName "$name.cat"
    if (!(Test-Path -LiteralPath $cat -PathType Leaf)) { throw "Missing catalog for $name" }

    Get-FileHash -Algorithm SHA256 -LiteralPath $inf,$sys,$cat |
        Select-Object Algorithm,Hash,@{Name='File';Expression={Split-Path -Leaf $_.Path}} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output "$name.sha256.json")
}
