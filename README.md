# Keychain 1.0

A local, resident radial launcher for Windows 11. Hold **Win**, press and hold **Caps Lock**, move toward an action, release **Caps Lock** to commit. Release over the center to cancel. **Escape** cancels. **Right-click** goes back from a subring (or cancels at the root).

Keychain renders with Direct3D 12 + DirectComposition as genuinely transparent, premultiplied-alpha glass — a bounded ring surface, not a full-screen overlay — and performs **zero periodic rendering while hidden** (measured: 0 frames and 15.6 ms of process CPU across a 60-second hidden idle).

## Highlights

- Immediate keyboard activation via a low-level keyboard hook on a dedicated thread (allocation-free callback); Caps Lock's preexisting toggle state is preserved, consumed events are suppressed, and ordinary typing is untouched.
- Midnight Glass visuals: translucent charcoal sectors, ice-blue accent, procedural SDF ring geometry, DirectWrite grayscale glyph atlas, shell icon cache — all precompiled/prewarmed at startup, never on activation.
- Six- and eight-sector rings, one level of subrings (250 ms hover dwell or click to enter), deterministic edge/corner fan layouts, dead-zone center, angular hysteresis.
- Safe, typed actions: launch app/.lnk, open file/folder, open http(s) URL, send a key chord (injected only after you physically release the activation modifiers, with foreground revalidation), allowlisted Windows actions, open settings, subring navigation. No cmd.exe/PowerShell routing; imported executable actions start unapproved until you review them.
- Complete WinUI 3 settings app (General / Hotring / Actions and profiles / Appearance and motion / Performance and diagnostics) with a radial assignment editor and a **live preview** that drives the real DX12 overlay in non-executing mode.
- Per-user install, no admin required; uninstall preserves your configuration unless you explicitly ask otherwise.

## Install

- **Installer** (recommended): run `Keychain-Setup-1.0.0-x64.exe`. Installs per-user under `%LOCALAPPDATA%\Programs\Keychain` with Start-menu shortcuts. No elevation prompt.
- **Portable**: unzip `Keychain-1.0.0-win-x64.zip` anywhere and run `Keychain.exe`.

Launch `Keychain.exe` once to start the resident launcher (tray icon: Open Settings / Active Profile / Pause / Reset input hooks / Exit). Opt into "Launch at startup" in Settings → General.

## Default ring

Documents · Downloads · File Explorer · Notepad · Windows Settings · Keychain Settings (with a sample subring: Task Manager, Copy, Paste, …). Unavailable targets render disabled and never execute something else.

## Alternative activation

Settings → Hotring lets you switch to **Ctrl+Alt+Space** (a real `RegisterHotkey`, checked for conflicts) and a **Toggle** mode with arrow-key navigation, Enter to commit, Escape/back navigation — the same menu is exposed to UI Automation/screen readers.

## Troubleshooting

- If the hotring stops responding to Win+Caps (e.g., after a third-party tool unloaded our hook), use the tray command **Reset & reinstall input hooks** — a real reinstall of the low-level hooks, not a status claim.
- Ctrl+Alt+Space conflicts are probed and reported in Settings with guidance.
- Diagnostics export (Settings → Performance and diagnostics) writes a redacted-by-default JSON report; user paths are masked unless you disable redaction.

## Building

See `BUILD.md`. In short: CMake + MSVC for the native launcher/tests and `dotnet build` for the settings app; `scripts\build.ps1`, `scripts\test.ps1`, `scripts\package.ps1`, `scripts\capture-performance.ps1` automate the whole chain.

## Verification, limits, and honesty

`VERIFICATION.md` maps every brief requirement to concrete evidence (tests, pixel checks, measured numbers) and lists what was **not** verified in this environment: the live physical Win+Caps hardware path was covered by deterministic tests but not exercised end-to-end here — after installing, do a 30-second manual smoke test: hold Win, hold Caps, move, release over an action; press Escape mid-gesture; right-click in a subring; confirm no Start-menu popup and no stuck Caps Lock state. Photon latency, multi-monitor/mixed-DPI, and clean-machine installs are likewise labeled unverified. Artifacts are unsigned (SHA-256 sums in `dist\SHA256SUMS.txt`).

## Repository layout

```
src/native       C++20 launcher (app, input, gesture, layout, animation, graphics, actions, config, ipc, diagnostics)
src/shaders      HLSL (ring SDF, atlas quads) — compiled to assets/shaders via DXC at build time
settings         WinUI 3 settings app (unpackaged, self-contained)
tests            native test suite + contract fixture
scripts          build / test / package / capture-performance
installer        Inno Setup script (per-user)
docs             this file, BUILD.md, ARCHITECTURE.md, VERIFICATION.md, CHANGELOG.md
```

Keychain is a local tool: no cloud accounts, no telemetry, no network activity except when you explicitly open a URL action.
