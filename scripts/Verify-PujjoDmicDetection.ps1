# SPDX-License-Identifier: BSD-3-Clause
# Read-only: never installs or modifies the driver, certificate stores or registry.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SubmissionDirectory,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-fA-F]{40}$')] [string]$ExpectedRevision,
    [string]$SignedDirectory,
    [string]$SignToolPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Assert([bool]$OK, [string]$Why) { if (!$OK) { throw "Detection preflight FAILED: $Why" } }
$thumb = '9B7476DECA7CA559F9B77CA9DEE19E38B9A9E9E5'
$root = (Resolve-Path -LiteralPath $SubmissionDirectory).Path
$m = Get-Content -LiteralPath (Join-Path $root 'sha256.json') -Raw | ConvertFrom-Json
Assert ($m.Version -ceq '0.0.8.1' -and $m.Revision -eq $ExpectedRevision) 'Wrong version or revision'
$expected = @('parent/sof_dsp.inf','parent/sof_dsp.sys','parent/sof_dsp.cat',
              'child/pujjo_audio_detection.inf','child/pujjo_audio_detection.sys','child/pujjo_audio_detection.cat')
Assert (@($m.Files).Count -eq $expected.Count) 'Unexpected manifest entry count'
foreach ($relative in $expected) {
    $entry = @($m.Files | Where-Object { $_.File -ceq $relative })
    Assert ($entry.Count -eq 1) "Wrong manifest entry: $relative"
    Assert ($entry[0].Hash -match '^[0-9A-Fa-f]{64}$') "Invalid digest: $relative"
    $path = Join-Path $root $relative
    Assert ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $entry[0].Hash) "Digest mismatch: $relative"
    if ($relative.EndsWith('.inf')) {
        $content = Get-Content -LiteralPath $path -Raw
        Assert ($content -match '(?im)^DriverVer\s*=\s*11/26/2026,0\.0\.8\.1\s*$') "Wrong INF version: $relative"
    }
}
foreach ($dir in @('parent','child')) {
    Assert (@(Get-ChildItem -Force -LiteralPath (Join-Path $root $dir)).Count -eq 3) "Unexpected submission file in $dir"
}
$parent = Get-Content -Raw -LiteralPath (Join-Path $root 'parent/sof_dsp.inf')
$child = Get-Content -Raw -LiteralPath (Join-Path $root 'child/pujjo_audio_detection.inf')
Assert ($parent -match '(?im)^%Pujjo\.DeviceDesc%\s*=\s*Sof_Device, PCI\\VEN_8086&DEV_54C8&SUBSYS_00000000\s*$') 'Wrong PCI identity'
foreach ($name in @('SafeBootPolicy','BootSafetyToken','ArmedTesting','EnablePortCls','EnableS0Idle','EnableUsbOffload')) {
    Assert ($parent -match "(?im)^HKR,Parameters,$name,0x00010001,0\s*$") "Unsafe default: $name"
}
Assert ($child -match '(?im)^%Device%=Dmic,PUJJO\\AudioDmicDetectionTest\s*$') 'Wrong detection child identity'
Assert ($child -match '(?im)^HKR,EP\\0,%PKEY_AudioDevice_NeverSetAsDefaultEndpoint%,0x00010001,0x00000307\s*$') 'No automatic-default exclusion'
Write-Host 'PASS: unsigned submission identity, exact files, hashes and detection-only INF defaults.'
if (!$SignedDirectory) {
    Write-Host 'STOP: unsigned preflight only; do not install these catalog files.'
    return
}
Assert ($SignToolPath -and (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) 'SignTool is needed on the signing VM only'
foreach ($spec in @(@{ Dir='parent'; Base='sof_dsp' },@{ Dir='child'; Base='pujjo_audio_detection' })) {
    $dir = Join-Path $SignedDirectory $spec.Dir
    Assert (@(Get-ChildItem -Force -LiteralPath $dir).Count -eq 3) "Unexpected signed file in $dir"
    $cat = Join-Path $dir ($spec.Base + '.cat')
    $sig = Get-AuthenticodeSignature -LiteralPath $cat
    Assert ($sig.Status -eq 'Valid') "Catalog invalid: $cat"
    Assert ($sig.SignerCertificate.Thumbprint -eq $thumb) "Wrong signer: $cat"
    & $SignToolPath verify /v /pa $cat
    Assert ($LASTEXITCODE -eq 0) "Catalog trust failure: $cat"
    foreach ($ext in @('inf','sys')) {
        $file = $spec.Base + '.' + $ext
        $signedFile = Join-Path $dir $file
        $originalFile = Join-Path (Join-Path $root $spec.Dir) $file
        Assert ((Get-FileHash -LiteralPath $signedFile).Hash -eq (Get-FileHash -LiteralPath $originalFile).Hash) "Changed binary or INF: $file"
        & $SignToolPath verify /v /pa /c $cat $signedFile
        Assert ($LASTEXITCODE -eq 0) "Catalog membership failure: $file"
    }
}
Write-Host "PASS: both catalogs locally test-signed and members verified. Revision $ExpectedRevision."
Write-Host 'Hardware enumeration and Microsoft release signing are separate gates. No device was changed.'
