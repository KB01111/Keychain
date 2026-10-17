# scripts/package.ps1 - stages the portable layout, creates the portable zip and
# the per-user Inno Setup installer, and writes SHA-256 checksums.
# Artifacts:
#   dist\Keychain-1.0.0-win-x64\        (portable folder)
#   dist\Keychain-1.0.0-win-x64.zip     (portable archive)
#   dist\Keychain-Setup-1.0.0-x64.exe   (per-user installer, no admin required)
#   dist\SHA256SUMS.txt
[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$version = '1.0.0'
$dist = Join-Path $repoRoot 'dist'
$portableDir = Join-Path $dist "Keychain-$version-win-x64"
$zipPath = Join-Path $dist "Keychain-$version-win-x64.zip"
$installerPath = Join-Path $dist "Keychain-Setup-$version-x64.exe"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) {
        Write-Error 'Build failed; cannot package.'
        exit 1
    }
}

# --- Locate build outputs (fail clearly if missing) ---------------------------
$nativeRelease = Join-Path $repoRoot 'build\vs2022-x64\Release'
$settingsOut = Join-Path $repoRoot 'settings\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64'
$vcCrt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'

foreach ($p in @(
    (Join-Path $nativeRelease 'Keychain.exe'),
    (Join-Path $nativeRelease 'assets\shaders\ring_sdf_vs.cso'),
    (Join-Path $settingsOut 'Keychain.Settings.exe'),
    $vcCrt
)) {
    if (-not (Test-Path $p)) {
        Write-Error "Missing required build/packaging input: $p"
        exit 1
    }
}

# --- Stage portable layout -----------------------------------------------------
if (Test-Path $portableDir) { Remove-Item -Recurse -Force $portableDir }
New-Item -ItemType Directory -Force -Path $portableDir | Out-Null

Copy-Item (Join-Path $nativeRelease 'Keychain.exe') $portableDir
Copy-Item (Join-Path $nativeRelease 'assets') (Join-Path $portableDir 'assets') -Recurse

# Settings app + app-local Windows App SDK / .NET self-contained runtime
Copy-Item $settingsOut (Join-Path $portableDir 'settings') -Recurse

# VC++ runtime DLLs required by the native launcher (dynamic CRT)
Copy-Item (Join-Path $vcCrt '*.dll') $portableDir

Write-Host "==> Staged portable layout: $portableDir"

# --- Portable zip ---------------------------------------------------------------
Write-Host '==> Creating portable zip...'
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $portableDir '*') -DestinationPath $zipPath
Write-Host "  $zipPath ($((Get-Item $zipPath).Length) bytes)"

# --- Inno Setup installer (per-user, nonadministrative [R17]) -------------------
Write-Host '==> Building Inno Setup installer...'
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) {
    Write-Error "Inno Setup 6 not found at '$iscc'. Install Inno Setup 6 (user scope) to build the installer."
    exit 1
}
$iss = Join-Path $repoRoot 'installer\Keychain.iss'
if (Test-Path $installerPath) { Remove-Item -Force $installerPath }
& $iscc "/DSourceRoot=$portableDir" "/DAppVersion=$version" $iss
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $installerPath)) {
    Write-Error 'Inno Setup installer build failed.'
    exit 1
}
Write-Host "  $installerPath ($((Get-Item $installerPath).Length) bytes)"

# --- SHA-256 checksums ----------------------------------------------------------
$hashes = foreach ($f in @($zipPath, $installerPath)) {
    $h = Get-FileHash $f -Algorithm SHA256
    '{0}  {1}' -f $h.Hash, (Split-Path -Leaf $f)
}
$hashes | Set-Content (Join-Path $dist 'SHA256SUMS.txt') -Encoding ascii
Write-Host "  SHA-256 sums written to $dist\SHA256SUMS.txt"

Write-Host ''
Write-Host 'Packaging succeeded.' -ForegroundColor Green
Write-Host "  Portable folder : $portableDir"
Write-Host "  Portable zip    : $zipPath"
Write-Host "  Installer       : $installerPath"
