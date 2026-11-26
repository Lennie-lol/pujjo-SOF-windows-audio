# SPDX-License-Identifier: BSD-3-Clause
# Hardware-free control-flow tests. SignTool and Windows trust results are simulated.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
$ci = New-Item -ItemType Directory -Path (Join-Path $root 'submission') -Force
$signed = New-Item -ItemType Directory -Path (Join-Path $root 'signed')
try {
    $revision = 'a' * 40
    $thumb = '9B7476DECA7CA559F9B77CA9DEE19E38B9A9E9E5'
    $trust = 'Valid'
    function Get-AuthenticodeSignature([string]$LiteralPath) {
        if ($LiteralPath.EndsWith('.sys')) { return [pscustomobject]@{Status='NotSigned'} }
        return [pscustomobject]@{Status=$trust; SignerCertificate=@{Thumbprint=$thumb}}
    }
    Copy-Item (Join-Path $repo 'packaging/diagnostics/pujjo_dsp_boot_only.inx') (Join-Path $ci.FullName 'sof_dsp.inf')
    Set-Content (Join-Path $ci.FullName 'sof_dsp.sys') 'fixture SYS'
    Set-Content (Join-Path $ci.FullName 'sof_dsp.cat') 'fixture CAT'
    Get-ChildItem $ci.FullName | Copy-Item -Destination $signed.FullName
    Get-ChildItem $ci.FullName | Get-FileHash -Algorithm SHA256 |
        Select-Object Algorithm,Hash,@{Name='File';Expression={Split-Path -Leaf $_.Path}} |
        ConvertTo-Json | Set-Content (Join-Path $root 'sha256.json')
    Set-Content (Join-Path $root 'UNSIGNED-SUBMISSION-ONLY.txt') "Source revision $revision unsigned Package version 0.0.7.0; DMA and firmware execution locked"
    $tool = Join-Path $root 'verify.cmd'
    Set-Content -Encoding Ascii $tool @(
        '@echo off'
        'if not "%1 %2 %3"=="verify /pa /v" exit /b 2'
        'if /i "%~nx4"=="sof_dsp.cat" if exist "%~4" if "%~5"=="" exit /b 0'
        'if not "%4"=="/c" exit /b 3'
        'if /i not "%~nx5"=="sof_dsp.cat" exit /b 4'
        'if exist "%~dp0%~nx6.fail" exit /b 1'
        'if "%~nx6"=="sof_dsp.inf" if exist "%~6" exit /b 0'
        'if "%~nx6"=="sof_dsp.sys" if exist "%~6" exit /b 0'
        'exit /b 5'
    )
    $params = @{SubmissionDirectory=$root; ExpectedRevision=$revision; SignedPackageDirectory=$signed.FullName; SignToolPath=$tool}
    function Verify { & (Join-Path $repo 'scripts/Verify-PujjoLocalDiagnostic.ps1') @params }
    function ExpectFailure([string]$message) {
        try { Verify | Out-Null } catch {
            if ($_.Exception.Message -like "*$message*") { return }
            throw
        }
        throw "Unexpected acceptance: $message"
    }
    Verify
    $trust = 'NotTrusted'
    ExpectFailure 'pinned development certificate'
    $trust = 'Valid'
    $thumb = '0' * 40
    ExpectFailure 'pinned development certificate'
    $thumb = '9B7476DECA7CA559F9B77CA9DEE19E38B9A9E9E5'
    foreach ($name in 'sof_dsp.inf','sof_dsp.sys') {
        $marker = Join-Path $root "$name.fail"
        New-Item $marker -ItemType File | Out-Null
        ExpectFailure "membership failed for $name"
        Remove-Item -LiteralPath $marker
        Add-Content (Join-Path $signed.FullName $name) 'tampered'
        ExpectFailure "modified $name"
        Copy-Item (Join-Path $ci.FullName $name) $signed.FullName -Force
    }
    # Verify the restored package and leave the native exit status at success.
    Verify
    Write-Output 'PASS: local signing control-flow regressions'
} finally {
    if ((Split-Path $root -Parent).TrimEnd('\') -eq [IO.Path]::GetTempPath().TrimEnd('\')) {
        Remove-Item -LiteralPath $root -Force -Recurse
    }
}
