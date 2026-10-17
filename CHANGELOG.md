# Changelog

## 1.0.0 — 2026-10-18

Initial release.

### Added

- Resident Win+Caps radial launcher (`Keychain.exe`): low-level keyboard hook on a dedicated thread, gesture state machine with generation IDs, exactly-once dispatch, Caps Lock toggle-state preservation, Win-key Start-menu masking scoped to recognized gestures, Ctrl+Alt+Space alternative hotkey, pause mode, foreground-executable exclusions.
- Direct3D 12 + DirectComposition overlay: genuinely transparent premultiplied-alpha composition swap chain, procedural SDF Midnight Glass ring, DirectWrite grayscale glyph atlas, shell icon cache, 6/8-sector layouts, edge/corner fans, subrings (one level, 250 ms dwell), compositor-clock pacing with paired DRR boost, zero rendering while hidden, device-loss recovery, PNG self-test capture.
- Typed safe actions: launch app/.lnk, open file/folder, http(s) URL, deferred key-chord injection (modifier release + foreground revalidation + timeout), allowlisted Windows actions, Keychain settings, subring navigation, no-op. Import marks executable actions unapproved; preview never executes.
- Complete WinUI 3 settings app (General / Hotring / Actions & profiles / Appearance & motion / Performance & diagnostics) with radial assignment editor, draft-based Apply flow, live non-executing DX12 preview streaming, diagnostics export, input-hook reset.
- Schema-v1 configuration with atomic persistence, last-known-good recovery, and a shared native↔managed contract fixture; per-session named-pipe IPC with explicit security descriptor.
- Native test suite: 24 doctest cases covering gesture ordering/cancellation, deferred shortcuts, queue overflow, layout (6/8 sectors, all fans, negative coordinates), spring stability 60–500 Hz, config validation/recovery/import safety, IPC round-trips, a 1,000-cycle activation stress test with handle/memory growth checks, and a real D3D12 composition-frame test.
- Build/test/package/capture scripts (`scripts/`), per-user Inno Setup installer (config-preserving uninstall), portable zip, SHA-256 checksums.
- Documentation: README, BUILD, ARCHITECTURE, VERIFICATION (requirement-to-evidence record).

### Notes

- Artifacts are unsigned; verify with `dist\SHA256SUMS.txt`.
- Engineering targets not reached on the reference machine are recorded honestly in `VERIFICATION.md` (hotkey-to-first-submission and active-frame CPU p95; GPU p95, hidden-idle, and resident-memory targets met).
