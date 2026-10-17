# scripts/build.ps1 - builds the native Keychain launcher (CMake + MSVC + DXC shaders)
# and the Keychain.Settings WinUI 3 application (dotnet).
# Prerequisites: CMake 3.24+, Windows SDK (dxc.exe), MSVC (VS2022+), .NET SDK 8+.
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipSettings
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build\vs2022-x64'

# --- Prerequisite discovery (fail clearly on missing tools) ------------------
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
    $cmakeCmd = Get-Command 'C:\Program Files\CMake\bin\cmake.exe' -ErrorAction SilentlyContinue
}
if (-not $cmakeCmd) {
    Write-Error 'cmake not found. Install CMake 3.24+ (https://cmake.org) and ensure it is on PATH.'
    exit 1
}

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Error 'dotnet SDK not found. Install the .NET SDK 8.0 or newer (https://dotnet.microsoft.com).'
    exit 1
}

$dxcFound = $false
foreach ($kit in @('10.0.26100.0', '10.0.28000.0', '10.0.22621.0', '10.0.20348.0')) {
    if (Test-Path "C:\Program Files (x86)\Windows Kits\10\bin\$kit\x64\dxc.exe") { $dxcFound = $true; break }
}
if (-not $dxcFound) {
    Write-Error 'dxc.exe (DirectX Shader Compiler, part of the Windows SDK) not found under C:\Program Files (x86)\Windows Kits\10.'
    exit 1
}

# --- Native configure (only when needed) ------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host '==> Removing previous native build directory...'
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Host '==> Configuring native build (CMake preset windows-x64-vs2022)...'
    & $cmakeCmd.Source --preset windows-x64-vs2022
    if ($LASTEXITCODE -ne 0) {
        Write-Error 'CMake configure failed.'
        exit 1
    }
}

# --- Native build -------------------------------------------------------------
Write-Host '==> Building native targets (release preset: KeychainCore, Keychain, Keychain.Tests, shaders)...'
& $cmakeCmd.Source --build --preset release
if ($LASTEXITCODE -ne 0) {
    Write-Error 'Native build failed.'
    exit 1
}

$nativeExe = Join-Path $buildDir 'Release\Keychain.exe'
if (-not (Test-Path $nativeExe)) {
    Write-Error "Native build reported success but $nativeExe is missing."
    exit 1
}

# --- Settings app build -------------------------------------------------------
if (-not $SkipSettings) {
    Write-Host '==> Building Keychain.Settings (WinUI 3, x64 Release)...'
    Push-Location (Join-Path $repoRoot 'settings')
    try {
        dotnet build Keychain.Settings.csproj -c Release -p:Platform=x64
        if ($LASTEXITCODE -ne 0) {
            Write-Error 'Keychain.Settings build failed.'
            exit 1
        }
    }
    finally {
        Pop-Location
    }
}

Write-Host ''
Write-Host 'Build succeeded.' -ForegroundColor Green
Write-Host "  Native launcher : $nativeExe"
if (-not $SkipSettings) {
    Write-Host "  Settings app    : $repoRoot\settings\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\Keychain.Settings.exe"
}
