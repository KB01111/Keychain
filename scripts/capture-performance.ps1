# scripts/capture-performance.ps1 - captures performance evidence:
#   1. The launcher's built-in benchmark (hidden-idle verification, warm
#      hotkey-to-first-submission latency, sustained animation, settled-visible
#      behavior, memory/handles) -> benchmark-report.json
#   2. Optional PresentMon trace (when a PresentMon build is present) recorded
#      while a second benchmark pass runs, -> presentmon.csv
[CmdletBinding()]
param(
    [int]$IdleSeconds = 60,
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'verification\performance' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$exe = Join-Path $repoRoot 'build\vs2022-x64\Release\Keychain.exe'
if (-not (Test-Path $exe)) {
    Write-Error "Keychain.exe not found at $exe. Run scripts\build.ps1 first."
    exit 1
}

# --- 1. Built-in benchmark -------------------------------------------------------
$report = Join-Path $OutputDir 'benchmark-report.json'
Write-Host "==> Running built-in benchmark (idle verification ${IdleSeconds}s + latency + animation phases)..."
# Keychain.exe is a GUI-subsystem executable: `&` does not wait for it.
$benchProc = Start-Process -FilePath $exe -ArgumentList '--benchmark', $report, '--idle-seconds', "$IdleSeconds" -Wait -PassThru
$benchExit = $benchProc.ExitCode
if ($benchExit -ne 0 -or -not (Test-Path $report)) {
    Write-Error "Benchmark failed (exit $benchExit, report: $report)."
    exit 1
}
Write-Host "  Benchmark report: $report ($((Get-Item $report).Length) bytes)"

# --- 2. Optional PresentMon trace -------------------------------------------------
$presentMon = $null
foreach ($candidate in @(
    'C:\Program Files\AMD\CNext\CNext\PresentMon-x64.exe',
    'C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\PresentMon_x64.exe'
)) {
    if (Test-Path $candidate) { $presentMon = $candidate; break }
}

if ($presentMon) {
    Write-Host "==> PresentMon found: $presentMon"
    $csv = Join-Path $OutputDir 'presentmon.csv'
    if (Test-Path $csv) { Remove-Item -Force $csv }

    # Classic PresentMon CLI: record Keychain.exe for ~30s while a short benchmark runs.
    $pm = Start-Process $presentMon -ArgumentList @(
        '-process_name', 'Keychain.exe',
        '-output_file', "`"$csv`"",
        '-timed', '30'
    ) -PassThru
    Start-Sleep -Seconds 2

    $shortReport = Join-Path $OutputDir 'benchmark-report-short.json'
    $shortProc = Start-Process -FilePath $exe -ArgumentList '--benchmark', $shortReport, '--idle-seconds', '2' -Wait -PassThru
    Write-Host "  Short benchmark pass exit: $($shortProc.ExitCode)"

    if (-not $pm.HasExited) {
        Write-Host '  Waiting for PresentMon to finish its 30s window...'
        Wait-Process -Id $pm.Id -Timeout 45 -ErrorAction SilentlyContinue
        if (-not $pm.HasExited) { Stop-Process -Id $pm.Id -Force }
    }
    if (Test-Path $csv) {
        Write-Host "  PresentMon trace: $csv ($((Get-Item $csv).Length) bytes)"
    } else {
        Write-Warning 'PresentMon produced no CSV (command-line options may differ on this build). Skipping trace evidence.'
    }
} else {
    Write-Warning 'No PresentMon installation found (AMD/NVIDIA paths checked). Only the built-in benchmark report was captured.'
}

Write-Host ''
Write-Host 'Performance capture finished.' -ForegroundColor Green
Write-Host "  Output directory: $OutputDir"
