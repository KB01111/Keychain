# Keychain 1.0 — Architecture

## Processes

| Process | Runtime | Role |
|---|---|---|
| `Keychain.exe` | C++20, Win32, D3D12 + DXGI + DirectComposition + DirectWrite + WIC | Resident launcher: input hooks, gesture state machine, layout, animation, rendering, action dispatch, config ownership, IPC server, tray, UIA provider |
| `Keychain.Settings.exe` | C#, WinUI 3 (unpackaged, self-contained) | Settings UI only; talks to the launcher over a named pipe; never writes configuration files itself |

The native process owns configuration validation, persistence, gesture state, action dispatch, and rendering. The settings process is a pure IPC client: every change it applies goes through `ApplyConfig` on the pipe, is validated by the native process, and is persisted atomically by the native process. The managed runtime is never in the input or rendering path.

## Native module map (src/native)

```
common/      types.h                 geometry, angles, DPI helpers, UTF-8↔UTF-16, UniqueHandle, IClock/SystemQpcClock/DeterministicTestClock
config/      config_schema.h, config_store.cpp   schema v1 model, bounds validation, atomic save + last-known-good recovery, import unapprove
input/       input_manager.*         WH_KEYBOARD_LL + WH_MOUSE_LL on a dedicated thread; allocation-free callbacks; BoundedInputQueue<256>; Ctrl+Alt+Space RegisterHotkey; Start-menu mask (self-tagged); hook reinstall
gesture/     gesture_state_machine.* Idle/Opening/Active/Closing/Suspended + deferred-shortcut pending states; generation IDs; snapshot-immutability; selection/cancel/commit rules
layout/      radial_layout.*         6/8-sector geometry, dead zone, edge/corner fans, atan2 hit test, hysteresis, flick continuity
animation/   spring_animator.*       velocity-preserving damped springs (60–500 Hz stable), shortest-path highlight, reduced-motion snap
graphics/    text_icon_atlas.*      DirectWrite glyph atlas (grayscale AA), shell icon atlas (WIC), procedural SDF icon fallbacks
             d3d12_overlay_renderer.*  adapter selection, device/queue, composition swap chain, DComp visual, PSOs, per-frame contexts, upload heaps, compositor clock + DRR boost, PNG capture
actions/     action_dispatcher.*     COM STA worker; typed execution; SendShortcut injection with foreground revalidation; allowlisted Windows actions; preview suppression
ipc/         ipc_server.*           per-session named pipe, length-prefixed JSON v1, session+user security descriptor, reject-remote
diagnostics/ diagnostics_manager.*  QPC latency/frame percentiles, bounded 100-entry history, path redaction, JSON export
app/         launcher_app.*          window/tray/lifecycle, wake-event main loop, renderer driving, handlers
             accessibility_provider.* IRawElementProviderSimple + WinEvents for the overlay
main.cpp     wWinMain, CLI (--self-test-render / --benchmark), single-instance
```

## Threading model

- **Main thread** (STA, DPI-aware): window, tray, gesture consumption, animation, rendering, IPC handlers run-to-completion under a mutex only for preview state.
- **Hook thread** (dedicated message pump): `WH_KEYBOARD_LL`/`WH_MOUSE_LL` callbacks do state bookkeeping, narrowly scoped suppression, and enqueue into `BoundedInputQueue<256>` (SRW lock, pointer-move coalescing, overflow → safe-cancel event). No allocation, no I/O, no COM, no rendering in callbacks. Modifier state is tracked from events; `GetAsyncKeyState` is never used to infer the arriving event.
- **Action thread** (COM STA): ShellExecuteEx, known-folder resolution, SendInput chord injection after event-driven modifier release + foreground revalidation (1500 ms bound), UIPI respected.
- **IPC thread**: overlapped pipe I/O with a stop event; requests parsed and dispatched to handlers with bounded timeouts.

Shutdown order: quit flag → wake → IPC stop → hook stop → gesture reset → dispatcher stop → tray removal → session notification unregistered → window destroyed → renderer shutdown (boost released first, uploads unmapped) → COM released.

## Rendering pipeline (D3D12 + DirectComposition)

1. Adapter selection: explicit override (name or LUID) if configured and compatible, else the display-driving / minimum-power hardware adapter that supports FL 12_0 + SM 6_0 — sleeping discrete GPUs are not woken.
2. One `ID3D12Device`, one `DIRECT` command queue, one composition swap chain: `IDXGIFactory2::CreateSwapChainForComposition` on the queue, 2 buffers, `B8G8R8A8_UNORM`, `FLIP_SEQUENTIAL`, `STRETCH`, **premultiplied alpha**, waitable when supported (`SetMaximumFrameLatency(1)`).
3. `DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcompDevice))` → `IDCompositionDesktopDevice` → target + visual → `SetContent(swapChain)` → commit.
4. Two frame contexts (allocator + fence + persistently mapped 256 B-aligned upload each). Barriers PRESENT→RT→PRESENT; clear {0,0,0,0}. Ring pass: 3-vertex full-surface procedural SDF (sectors, hub, shadow, rim, highlight, masks) via an embedded SM 6.0 CSO. Atlas pass: glyph/icon quads from prebuilt atlases. GPU timestamps via query heap with one-frame-delayed readback.
5. Pacing: `DCompositionWaitForCompositorClock` (when available) with the input-wake event and a waitable-swap-chain fallback; `DCompositionBoostCompositorClock` requested on activation and released on hide/cancel/error paths. When hidden the main loop blocks on `MsgWaitForMultipleObjectsEx(INFINITE)` — zero ticks, zero frames. When visible-but-settled, frames are submitted only on state change.
6. Window: bounded `WS_POPUP` with `WS_EX_NOREDIRECTIONBITMAP|TOPMOST|TOOLWINDOW|NOACTIVATE` (never `WS_EX_LAYERED`), shown without activation, circular `WM_NCHITTEST` region, `WM_GETOBJECT` → UIA provider. Device loss: cancel gesture, rebuild (bounded 3 attempts), then stop rendering with an actionable diagnostic.

## Gesture state machine

`Idle → Opening → Active → Closing → Idle` plus `Suspended` and a deferred-shortcut pending chain (`WaitingForModifierRelease → ReadyToInject/Dispatched/Cancelled`). Each activation gets a generation ID; stale events can't dispatch. Win-down→Caps-down ordering required (Caps-first keeps ordinary Caps Lock); consumed Caps events are suppressed while preserving the preexisting toggle state. Commit on Caps-up uses the latest pointer position even before a render; center release, Escape, Win-first release, session loss, and invalid targets cancel. Subrings: 250 ms dwell or click to enter (one level only, cycle-checked), right-click returns. A decorative spring never changes logical selection — selection is computed by `RadialLayout::HitTest` (atan2, wrap-safe, hysteresis, flick continuity), and the animation layer only decorates it.

## Configuration and persistence

Schema v1 (`schemaVersion`, `revision`, `activeProfileId`, `preferences`, `profiles`, `rings`, `actions`) stored as UTF-8 JSON under `%LOCALAPPDATA%\Keychain` with atomic replace + `config.last_good.json`. Bounds: ≤512 KiB, ≤32 profiles / 128 rings / 512 actions, 6-or-8 sectors, max one child level, cycle rejection. Malformed/truncated files recover from backup without executing anything. In-flight gestures keep their own immutable snapshot revision. `tests/fixtures/config_v1.json` is the shared native↔managed contract fixture (generated by the native serializer).

## IPC protocol v1

Pipe `\\.\pipe\Keychain.Ipc.v1.<SessionId>`; 4-byte little-endian length prefix + UTF-8 JSON, 1 MiB cap, current user + logon-session SID security descriptor, `PIPE_REJECT_REMOTE_CLIENTS`, bounded timeouts, multi-exchange connections.

```
Request:  { "version": 1, "requestId": "<string>", "type": "<Type>", "payload": { ... } }
Response: { "version": 1, "requestId": ..., "type": "<Type>Response", "ok": bool,
            "error": "...", "errors": [], "warnings": [], "payload": { ... } }
Types: GetConfig | ApplyConfig | ImportConfig | PreviewStart | PreviewUpdate | PreviewStop |
       GetDiagnostics | ExportDiagnostics | ResetInput | Pause | Resume | ShowSettings
```

Preview streams the *draft* config to the real overlay in non-executing mode (action dispatch suppressed); the settings app debounces updates at 250 ms and stops on window close/timeout.

## Settings app

WinUI 3, Mica backdrop (solid fallback), `NavigationView` with five pages: General, Hotring, Actions & profiles (radial assignment editor + action CRUD + profile management + import/export/restore-defaults), Appearance & motion, Performance & diagnostics (live metrics, adapter/monitor info, exclusions, input reset, export). All controls commit to an in-memory draft; Apply validates client-side then sends `ApplyConfig`; the native side re-validates and answers with the new revision. Keyboard navigation and accessible names are wired throughout; the custom radial diagram exposes per-sector automation names.
