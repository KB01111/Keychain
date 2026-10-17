# scripts/test.ps1 - runs the deterministic native test suite (ctest) and the
# product render self-test that captures a real transparent overlay frame.
[CmdletBinding()]
param(
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build\vs2022-x64'

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Error 'No native build found. Run scripts\build.ps1 first.'
    exit 1
}

Write-Host '==> Running Keychain.Tests via ctest (Release)...'
ctest --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) {
    Write-Error 'Native test suite failed.'
    exit 1
}

if (-not $SkipSelfTest) {
    Write-Host '==> Product self-test: rendering a real transparent overlay frame...'
    $outDir = Join-Path $repoRoot 'verification\render'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $png = Join-Path $outDir 'selftest.png'
    if (Test-Path $png) { Remove-Item -Force $png }

    $exe = Join-Path $buildDir 'Release\Keychain.exe'
    # Keychain.exe is a GUI-subsystem executable: PowerShell's `&` invocation
    # does not wait for it and never sets $LASTEXITCODE. Wait explicitly.
    $proc = Start-Process -FilePath $exe -ArgumentList '--self-test-render', $png -Wait -PassThru
    if ($proc.ExitCode -ne 0 -or -not (Test-Path $png)) {
        Write-Error "Self-test render failed (exit $($proc.ExitCode), no PNG captured)."
        exit 1
    }
    $size = (Get-Item $png).Length
    Write-Host "  Self-test PNG captured: $png ($size bytes)"
}

Write-Host ''
Write-Host 'All tests passed.' -ForegroundColor Green
