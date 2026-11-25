# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SubmissionDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$ExpectedRevision,
    [Parameter(Mandatory)] [string]$SignedPackageDirectory,
    [Parameter(Mandatory)] [string]$SignToolPath,
    [string]$FirmwarePath,
    [switch]$InspectLocalBoard
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Run the same provenance, manifest, target and INF safety checks as release CI.
# The sibling script's optional signed-package path always enforces /kp.
& (Join-Path $PSScriptRoot 'Verify-PujjoDspBootDiagnostic.ps1') -SubmissionDirectory $SubmissionDirectory -ExpectedRevision $ExpectedRevision -FirmwarePath $FirmwarePath -InspectLocalBoard:$InspectLocalBoard

$expectedSigner = '9B7476DECA7CA559F9B77CA9DEE19E38B9A9E9E5'
$names = @('sof_dsp.inf', 'sof_dsp.sys', 'sof_dsp.cat')
$entries = @(Get-ChildItem -LiteralPath $SignedPackageDirectory -Force)
if ($entries.Count -ne $names.Count) { throw 'Unexpected signed package contents' }
foreach ($entry in $entries) {
    if ($entry.Name -notin $names -or $entry.PSIsContainer -or
        ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected signed package entry: $($entry.Name)"
    }
}
$original = Join-Path $SubmissionDirectory 'submission'
foreach ($name in 'sof_dsp.inf','sof_dsp.sys') {
    $ci = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $original $name)
    $local = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $SignedPackageDirectory $name)
    if ($ci.Hash -ne $local.Hash) { throw "Signed package modified $name" }
}
$catalog = Join-Path $SignedPackageDirectory 'sof_dsp.cat'
$signature = Get-AuthenticodeSignature -LiteralPath $catalog
if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $expectedSigner) {
    throw 'Catalog must have a trusted signature from the pinned development certificate'
}
$tool = (Get-Item -LiteralPath $SignToolPath).FullName
& $tool verify /pa /v $catalog
if ($LASTEXITCODE -ne 0) { throw 'Catalog failed local Authenticode policy' }
foreach ($name in 'sof_dsp.inf','sof_dsp.sys') {
    & $tool verify /pa /v /c $catalog (Join-Path $SignedPackageDirectory $name)
    if ($LASTEXITCODE -ne 0) { throw "Local catalog membership failed for $name" }
}
Write-Output "PASS: local test signature and membership only. Revision $ExpectedRevision. Signer $expectedSigner. Hardware review and Microsoft release signing are separate gates."
