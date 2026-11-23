# SPDX-License-Identifier: BSD-3-Clause
# Read-only offline DMIC-only package verifier. Never installs or signs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SubmissionDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$ExpectedRevision
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$Good, [string]$Why) {
    if (-not $Good) { throw "DMIC review FAILED: $Why" }
}

$root = (Resolve-Path -LiteralPath $SubmissionDirectory -ErrorAction Stop).ProviderPath
$dir = Join-Path $root 'submission'
$names = @('pujjo_audio.inf', 'pujjo_audio.sys', 'pujjo_audio.cat')
$files = @(Get-ChildItem -LiteralPath $dir -File -Force | Select-Object -ExpandProperty Name)
Require ($files.Count -eq 3) 'Exactly INF, SYS, CAT expected'
foreach ($name in $names) {
    Require ($files -contains $name) "Missing $name"
}

$notice = Get-Content -LiteralPath (Join-Path $root 'UNSIGNED-DMIC-REVIEW-ONLY.txt') -Raw
Require ($notice.Contains("Source revision $ExpectedRevision")) 'Wrong source revision'
Require ($notice.Contains('NOT approved for installation')) 'Unsigned warning absent'

$manifest = ConvertFrom-Json -InputObject (Get-Content -LiteralPath (Join-Path $root 'sha256.json') -Raw)
$records = New-Object 'System.Collections.Generic.List[object]'
foreach ($record in $manifest) { [void]$records.Add($record) }
Require ($records.Count -eq 3) 'Expected exactly three SHA-256 records'
foreach ($name in $names) {
    $hashRecords = @($records | Where-Object { $_.File -eq $name })
    Require ($hashRecords.Count -eq 1) "Duplicate or missing hash for $name"
    $record = $hashRecords[0]
    Require (([string]$record.Algorithm -eq 'SHA256') -and
             ([regex]::IsMatch([string]$record.Hash, '^[0-9a-fA-F]{64}$'))) "Invalid SHA-256 entry for $name"
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dir $name)).Hash
    Require ($actual -eq [string]$record.Hash) "Hash mismatch for $name"
}

$inf = Get-Content -LiteralPath (Join-Path $dir 'pujjo_audio.inf') -Raw
$required = @(
    'PUJJO\AudioDmic',
    'Include=ks.inf,wdmaudio.inf',
    'Needs=KS.Registration,WDMAUDIO.Registration',
    'AddService=pujjo_audio,0x00000002,Service',
    'AddInterface=%Capture%,DmicWave,WaveInterface',
    'AddInterface=%Topology%,DmicTopology,TopologyInterface',
    'ServiceBinary=%13%\pujjo_audio.sys'
)
foreach ($item in $required) {
    Require ($inf.Contains($item)) "Missing audio INF contract: $item"
}
foreach ($item in @('AudioSpeaker', 'AudioHeadphone', 'AudioHeadsetMic',
                    'PCI\VEN_8086', 'SafeBootPolicy', 'BootSafetyToken')) {
    Require (-not $inf.Contains($item)) "Unexpected non-DMIC target: $item"
}
Require ((Get-AuthenticodeSignature -LiteralPath (Join-Path $dir 'pujjo_audio.sys')).Status -eq
         'NotSigned') 'Expected unsigned review SYS'
Write-Output 'PASS: DMIC-only source, INF, hashes, and catalog. UNSIGNED; NOT INSTALLABLE.'
