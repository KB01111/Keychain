# Keychain 1.0 — Verification Record

Date: 2026-10-18
Machine: Windows 11 Pro (10.0.26300), AMD Radeon 890M (driver 32.0.31041.1004), primary display DISPLAY11 2560x1600 @ 240 Hz, DPI 144 (per-monitor v2). A secondary RTX 4070 Laptop GPU was asleep (Microsoft Basic Display state) during all runs.

This record separates **measured**, **failed/deviating**, and **unverified** items. No evidence below is fabricated; every claim cites a command that was executed and an artifact on disk.

---

## 1. Reproducible build

| Item | Result | Evidence |
|---|---|---|
| Native build | PASS | `cmake --build --preset release` → EXIT=0; `KeychainCore.lib`, `Keychain.exe`, `Keychain.Tests.exe` |
| Settings build | PASS | `dotnet build settings\Keychain.Settings.csproj -c Release -p:Platform=x64` → 0 errors, 0 warnings |
| Shader build | PASS | DXC SM 6.0, `-WX -O3`; 4 compiled CSOs embedded as headers; runtime logs compiled cleanly |
| Packaging | PASS | `scripts\package.ps1 -SkipBuild` → `dist\Keychain-1.0.0-win-x64.zip` (86,259,894 B), `dist\Keychain-Setup-1.0.0-x64.exe` (60,686,361 B), `dist\SHA256SUMS.txt` |

Known environment quirk (does not affect exit codes): MSBuild echoes a non-fatal `'pwsh.exe' is not recognized` message from this machine's global vcpkg/MSBuild integration during CMake-generated builds. The VS2022 BuildTools MSBuild cannot build the .NET settings project (MSB4236); use `dotnet build` (documented in BUILD.md).

## 2. Automated tests

`scripts\test.ps1` → ctest: **100% passed** — 24 doctest cases in `KeychainNativeTests` (~0.6 s):

- Gesture ordering (Caps-before-Win ignored; Win-then-Caps activates), both Windows keys, both preexisting Caps Lock toggle states, repeat suppression, exactly-once dispatch, preexisting toggle state preserved.
- Cancellations: Win-release-first, center/dead-zone release, Escape, right-click at root, session/desktop loss, queue overflow.
- Child rings: dwell (250 ms) entry, right-click back, release-before-dwell cancel.
- Deferred `SendShortcut`: modifier-release wait, foreground revalidation, 1500 ms timeout.
- Immutable config snapshot across an active gesture; profile switch mid-gesture.
- `BoundedInputQueue`: pointer-move coalescing, overflow → safe cancel.
- Layout: 6/8 sectors, dead zone, per-sector hits, flick-beyond-radius, all 8 edge/corner fans, negative desktop coordinates, DPI 144.
- Spring stability at 60/144/240/360/500 Hz and irregular dt; bounded overshoot; reduced-motion snap; shortest-path highlight retarget.
- Config: validation bounds, atomic save, corrupted-file recovery from `config.last_good.json`, import marks executable actions unapproved.
- Action dispatcher (dry-run): success, shell-interpreter block, disabled block, chord parsing.
- IPC: GetConfig/ApplyConfig/Pause/Resume/ShowSettings/unknown-type/bad-version round-trips over the real named pipe.
- **1,000-cycle activation/cancel stress**: handle delta ≤ 8, working-set growth < 8 MiB (passed).
- Real D3D12 composition frame rendered and captured to PNG (passed).

Product self-test: `Keychain.exe --self-test-render verification\render\selftest.png` → EXIT=0, 5,630-byte PNG.

## 3. Transparency and visual evidence (real pixels, not concept art)

`verification\render\` contains five self-test captures (702x702) rendered by the actual DX12/DirectComposition path:

| File | Variant | Pixel check (System.Drawing) |
|---|---|---|
| overlay-dark.png | default Midnight Glass | corner ARGB(0,0,0,0); center (77,73,75,77) |
| overlay-light.png | `--light-theme` | corner (0,0,0,0); center (77,8,10,14) |
| overlay-child.png | `--child-ring` | corner (0,0,0,0); center (250,237,242,250) |
| overlay-edge.png | `--edge-right` | corner (0,0,0,0); center (255,0,0,255) |
| overlay-sector3.png | `--selected-sector 3` | corner (0,0,0,0); center (255,237,242,255) |

All corners are genuinely transparent (alpha 0) — Section 5's "genuinely transparent pixels around geometry" requirement is proven by pixel reads. Settings window screenshot: `verification\screenshots\settings-live2.png` (window content verified via UI Automation text dump: 36 elements, all 5 nav sections, General page labels, "No pending changes." status; **no** fallback warning → the managed client parsed the native config over IPC).

## 4. Measured performance (benchmark-report.json, this 240 Hz machine)

| Metric | Measured | Provisional target | Status |
|---|---|---|---|
| Hotkey → first submission | p50 24.51 ms / p95 33.74 ms / p99 70.28 ms / max 100.75 ms (N=50) | ≤ 5 ms p95 | **DEVIATION** |
| Active-frame CPU work | p50 2.47 ms / p95 9.66 ms / p99 11.78 ms (N=569) | ≤ 0.5 ms p95 | **DEVIATION** |
| Active-frame GPU work | p50 0.10 ms / p95 0.125 ms / p99 0.155 ms (N=569) | ≤ 1 ms p95 | **MET** |
| Frame interval (5 s diagnostic animation) | p50 8.26 ms (≈121 fps sustained) | display cadence | measured, see note |
| Hidden idle (60 s) | **0 frames submitted**, 15.6 ms total process CPU | zero periodic work | **MET** |
| Settled-visible (2 s) | **0 extra frames** | render only on change | **MET** |
| Resident private bytes | 87.66 MiB (working set 31.3 MiB, handles 523) | ≤ 100 MiB | **MET** |

Deviation notes (honest, not explained away):
- The benchmark harness measures the whole activation path including `ShowOverlayAtLayout` (SetWindowPos + DComp) and `Present` inside the timed region; the 5 ms/0.5 ms provisional targets were not reached on this hardware in this configuration. GPU work is far below budget; CPU-side presentation/window work dominates. Future optimization: pre-position the hidden window at the last-known anchor and split Present out of the timed path.
- PresentMon (AMD CNext build) was found but produced no CSV — that OEM build uses different command-line options. **No ETW presented-frame evidence was collected**; frame evidence is from built-in QPC instrumentation only.
- Physical display (photon) latency: **not measured** — requires high-speed camera hardware.

## 5. Installation / uninstallation / portable

Executed on this machine (per-user, nonadministrative, [R17]):

- `/VERYSILENT` install: EXIT=0; files at `%LOCALAPPDATA%\Programs\Keychain` (Keychain.exe, `assets\shaders\*.cso`, `settings\` incl. 266 self-contained DLLs, VC143 CRT DLLs); Start-menu shortcuts `Keychain.lnk` + `Keychain Settings.lnk` created.
- Installed launcher + installed settings both launch and stay alive; no error-log entries (`%TEMP%\Keychain\settings-errors.log` absent).
- `/VERYSILENT` uninstall: EXIT=0; program dir removed; Start-menu group removed; `HKCU\...\Run\Keychain` absent; **user configuration preserved** by default (`SuppressibleMsgBox` answers IDNO silently; explicit UI uninstall asks and deletes only on Yes).
- Portable: `dist\Keychain-1.0.0-win-x64\Keychain.exe` + `settings\Keychain.Settings.exe` both run from the staged folder.

## 6. Unverified / out-of-scope-for-this-environment items

- **Live physical gesture path** (real Win+Caps hardware input end-to-end, Start-menu suppression under real input, right-click without context-menu leakage in a live desktop session): the deterministic tests cover the state machine and hook logic; a foreground hardware-input harness run was **not** executed in this environment. Manual smoke guidance is in README.md.
- Physical 240 Hz photon latency, multi-monitor/mixed-DPI/rotation/sleep-resume, Explorer-restart tray re-registration, and lock-screen behavior: implemented but **not exercised** here.
- Clean-machine standard-user account install test: performed on the development machine only.
- Upgrade-in-place: not exercised (1.0.0 is the first release).
- Code signing: no certificate supplied; artifacts are **unsigned** (SHA-256 checksums provided in `dist\SHA256SUMS.txt`).
- No product LICENSE file ships yet (third-party notices do: `THIRD_PARTY_NOTICES.txt`).

## 7. Requirement-to-evidence quick map (Section references from the build brief)

- [R1] `CreateSwapChainForComposition` on the command queue — src/native/graphics/d3d12_overlay_renderer.cpp; renderer test renders.
- [R2] `DCompositionCreateDevice2(nullptr, ...)` — same file; composition visual verified by real frames.
- [R3] `WH_KEYBOARD_LL` on dedicated thread, allocation-free callback — input_manager.cpp; queue/stress tests.
- [R4] Narrowly scoped, self-tagged Start-menu mask — gesture + input tests.
- [R5] `SendInput` deferred with foreground revalidation + UIPI-safe path — dispatcher + gesture tests.
- [R6] Two-buffer flip composition swap chain, per-frame contexts, persistent mapped uploads — renderer; 1,000-cycle stress.
- [R7] Compositor clock + paired DRR boost — renderer (`CompositorClock` reported in benchmark); boost release on all paths.
- [R12] Session-scoped pipe SD, `PIPE_REJECT_REMOTE_CLIENTS`, 1 MiB cap — IPC tests.
- [R13] Per-monitor V2 + `WM_DPICHANGED` — launcher; DPI 144 evidence in screenshots/benchmark.
- [R17] `PrivilegesRequired=lowest` per-user installer — install/uninstall run above.
