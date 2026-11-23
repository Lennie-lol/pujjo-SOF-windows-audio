# SPDX-License-Identifier: BSD-3-Clause
# Read-only offline review. Does not install, sign or open any device.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SubmissionDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$ExpectedRevision
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Require([bool]$Good,[string]$Why) { if (!$Good) { throw "DMIC preflight FAILED: $Why" } }
$root=(Resolve-Path -LiteralPath $SubmissionDirectory -ErrorAction Stop).ProviderPath
$dir=Join-Path $root 'submission'
$names=@('pujjo_audio.inf','pujjo_audio.sys','pujjo_audio.cat')
$files=@(Get-ChildItem -LiteralPath $dir -File -Force | Select-Object -ExpandProperty Name)
Require ($files.Count -eq 3) 'Expected exactly INF/SYS/CAT'
foreach($name in $names) { Require ($files -contains $name) "Missing $name" }
$notice=Get-Content -LiteralPath (Join-Path $root 'UNSIGNED-DMIC-REVIEW-ONLY.txt') -Raw
Require ($notice.Contains("Source revision $ExpectedRevision")) 'Wrong revision'
Require ($notice.Contains('NOT approved for installation')) 'Safety notice missing'
$manifest=ConvertFrom-Json -InputObject (Get-Content -LiteralPath (Join-Path $root 'sha256.json') -Raw)
$records=New-Object 'System.Collections.Generic.List[object]'
foreach($record in $manifest) { [void]$records.Add($record) }
Require ($records.Count -eq 3) 'Expected three hash entries'
foreach($name in $names) {
    # Do not use $Matches: PowerShell's regex -match overwrites that
    # automatic variable, destroying our candidate record array.
    $hashRecords=@($records | Where-Object { $_.File -eq $name })
    Require ($hashRecords.Count -eq 1) "Bad hash record for $name"
    $entry=$hashRecords[0]
    Require (([string]$entry.Hash -match '^[0-9a-fA-F]{64}
}
$inf=Get-Content -LiteralPath (Join-Path $dir 'pujjo_audio.inf') -Raw
foreach($expected in @(
  'PUJJO\AudioDmic','Include=ks.inf,wdmaudio.inf',
  'Needs=KS.Registration,WDMAUDIO.Registration',
  'AddService=pujjo_audio,0x00000002,Service',
  'AddInterface=%Capture%,DmicWave,WaveInterface',
  'AddInterface=%Topology%,DmicTopology,TopologyInterface',
  'ServiceBinary=%13%\pujjo_audio.sys'
)) { Require ($inf.Contains($expected)) "Missing INF contract: $expected" }
foreach($unexpected in @('AudioSpeaker','AudioHeadphone','AudioHeadsetMic',
                          'PCI\VEN_8086','SafeBootPolicy','BootSafetyToken')) {
    Require (-not $inf.Contains($unexpected)) "Unexpected INF target: $unexpected"
}
Require ((Get-AuthenticodeSignature -LiteralPath (Join-Path $dir 'pujjo_audio.sys')).Status -eq
         'NotSigned') 'Review SYS must remain unsigned'
Write-Output 'PASS: DMIC-only unsigned review INF and package hashes. NOT INSTALLABLE.'
) -and
             $entry.Algorithm -eq 'SHA256') "Malformed hash for $name"
    Require ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dir $name)).Hash -eq
             [string]$entry.Hash) "Hash mismatch for $name"
}
$inf=Get-Content -LiteralPath (Join-Path $dir 'pujjo_audio.inf') -Raw
foreach($expected in @(
  'PUJJO\AudioDmic','Include=ks.inf,wdmaudio.inf',
  'Needs=KS.Registration,WDMAUDIO.Registration',
  'AddService=pujjo_audio,0x00000002,Service',
  'AddInterface=%Capture%,DmicWave,WaveInterface',
  'AddInterface=%Topology%,DmicTopology,TopologyInterface',
  'ServiceBinary=%13%\pujjo_audio.sys'
)) { Require ($inf.Contains($expected)) "Missing INF contract: $expected" }
foreach($unexpected in @('AudioSpeaker','AudioHeadphone','AudioHeadsetMic',
                          'PCI\VEN_8086','SafeBootPolicy','BootSafetyToken')) {
    Require (-not $inf.Contains($unexpected)) "Unexpected INF target: $unexpected"
}
Require ((Get-AuthenticodeSignature -LiteralPath (Join-Path $dir 'pujjo_audio.sys')).Status -eq
         'NotSigned') 'Review SYS must remain unsigned'
Write-Output 'PASS: DMIC-only unsigned review INF and package hashes. NOT INSTALLABLE.'
