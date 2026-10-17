# Keychain 1.0 — Build Guide

## Toolchain (as used and recorded for this build)

| Component | Version |
|---|---|
| OS | Windows 11 Pro, build 26300 |
| CMake | 4.3.3 (`C:\Program Files\CMake\bin\cmake.exe`) |
| Native compiler | MSVC 14.44.35207 (VS2022 Build Tools 17.14.37806.7) |
| Windows SDK | 10.0.26100.0 (DXC at `Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`) |
| .NET SDK | 10.0.300 / 10.0.402 (runtimes 8.0.32, 10.0.13) |
| Windows App SDK | 1.8.260921001 (NuGet, pinned in `settings/Keychain.Settings.csproj`) |
| Windows SDK Build Tools pkg | 10.0.26100.4654 |
| Inno Setup | 6.7.3 (user scope, `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`) |

Native dependencies are vendored and pinned in `third_party/dependencies.lock.json` with SHA-256 hashes: `nlohmann/json` 3.11.3 and `doctest` 2.4.11. No other third-party code is used; everything else is Windows SDK.

## Quick start (all-in-one)

```powershell
# from the repository root
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1     # native + settings
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1       # ctest + product self-test render
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1    # portable zip + installer + hashes
```

`build.ps1` verifies prerequisites (cmake, dotnet, dxc) and fails with a clear message if anything is missing. `-Clean` removes the build directory first; `-SkipSettings` skips the WinUI app.

## Manual commands

### Native (launcher, core, tests)

```powershell
cmake --preset windows-x64-vs2022          # configure (VS generator, x64); only needed once or after -Clean
cmake --build --preset release              # KeychainCore.lib + Keychain.exe + Keychain.Tests.exe
ctest --test-dir build\vs2022-x64 -C Release --output-on-failure
```

Output: `build\vs2022-x64\Release\Keychain.exe` and `Keychain.Tests.exe`. Shader bytecode is built by the `KeychainShaders` target (DXC `-WX -O3`, SM 6.0) and embedded as headers; the four `.cso` files are copied next to the exe for development runs.

### Settings app (WinUI 3)

```powershell
dotnet build settings\Keychain.Settings.csproj -c Release -p:Platform=x64
```

Output: `settings\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\Keychain.Settings.exe` (self-contained, unpackaged).

> **Important:** use `dotnet build` for the settings app. The standalone VS2022 **BuildTools MSBuild cannot resolve `Microsoft.NET.Sdk`** on this machine (MSB4236 — it lacks the .NET SDK resolvers). The full `dotnet` CLI drives MSBuild internally and works, including the WinUI XAML compiler.

### Product self-test render (real transparent overlay → PNG)

```powershell
$p = Start-Process -FilePath build\vs2022-x64\Release\Keychain.exe `
  -ArgumentList '--self-test-render','out.png','--selected-sector','3' -Wait -PassThru
$p.ExitCode   # 0 on success
```

Variants: `--light-theme`, `--child-ring`, `--edge-right`, `--selected-sector N`. **Always use `Start-Process -Wait`** for this GUI-subsystem executable — invoking it with `& exe` returns immediately and never sets `$LASTEXITCODE`.

### Benchmarks

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\capture-performance.ps1 -IdleSeconds 60
```

Runs `Keychain.exe --benchmark verification\performance\benchmark-report.json --idle-seconds 60` (hidden-idle frame/CPU verification, 50 warm activations, a 5-second sustained animation, settled-visible check, memory/handles) and optionally attempts a PresentMon trace if a compatible build is found (the OEM AMD CNext PresentMon on the reference machine uses nonstandard CLI options and produced no CSV — the script warns and continues).

### Packaging

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1 -SkipBuild   # reuse existing build outputs
```

Stages `dist\Keychain-1.0.0-win-x64\` (Keychain.exe, `assets\shaders\*.cso`, VC143 CRT DLLs, the complete self-contained `settings\` tree), produces `dist\Keychain-1.0.0-win-x64.zip`, compiles `dist\Keychain-Setup-1.0.0-x64.exe` with Inno Setup, and writes `dist\SHA256SUMS.txt`.

## Known build-environment quirks

- A non-fatal `'pwsh.exe' is not recognized` echo may appear during CMake-generated builds on machines with the global vcpkg/MSBuild integration (this machine has one). It does not affect build results; exit codes are authoritative. Do not "fix" user machine configuration for it.
- No `Directory.Build.props` exists above this repo; the echo comes from machine-level MSBuild integration, not from project files.
- The settings app is built **only** with `dotnet build` (see note above).

## What "verified build" means here

`VERIFICATION.md` records the exact commands executed, their exit codes, and the resulting artifacts. Builds are reproducible from a clean configure; tests and packaging were run end-to-end on the reference machine.
