#include "app/launcher_app.h"

#include <ShellScalingApi.h>
#include <WtsApi32.h>
#include <nlohmann/json.hpp>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace keychain {

namespace {

constexpr wchar_t kOverlayWindowClassName[] = L"KeychainRadialOverlayWindowClass";
constexpr UINT WM_KEYCHAIN_TRAY_CALLBACK = WM_APP + 101;
constexpr uint32_t kTrayIconId = 1;

constexpr int ID_TRAY_OPEN_SETTINGS = 4001;
constexpr int ID_TRAY_TOGGLE_PAUSE = 4002;
constexpr int ID_TRAY_RESET_INPUT = 4003;
constexpr int ID_TRAY_EXIT = 4004;
constexpr int ID_TRAY_PROFILE_BASE = 4100;
constexpr int kMaxTrayProfiles = 50;

constexpr int64_t kPreviewTimeoutUs = 45'000'000; // 45 seconds

RectI MonitorWorkAreaFromMonitor(HMONITOR hMonitor) noexcept {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hMonitor && GetMonitorInfoW(hMonitor, &mi)) {
        return RectI{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};
    }
    return RectI{0, 0, 1920, 1080};
}

uint32_t MonitorDpiFromMonitor(HMONITOR hMonitor, uint32_t fallbackDpi) noexcept {
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (hMonitor && SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
        return dpiX;
    }
    return fallbackDpi;
}

const wchar_t* FileNameFromPath(const wchar_t* path) noexcept {
    const wchar_t* fileName = path;
    for (const wchar_t* p = path; p && *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            fileName = p + 1;
        }
    }
    return fileName;
}

std::wstring GetSelfModuleDirectoryPath() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path().wstring();
}

} // namespace

LauncherApp::LauncherApp() = default;

LauncherApp::~LauncherApp() {
    Shutdown();
}

bool LauncherApp::AcquireSingleInstanceOrNotifyExisting(UniqueHandle& outMutex) {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    const std::wstring mutexName = L"Local\\Keychain.SingleInstance.v1." + std::to_wstring(sessionId);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) {
        outMutex.Reset(mutex);
        return true;
    }
    if (mutex) {
        CloseHandle(mutex);
    }

    // Already running: ask the primary instance to open its settings window.
    const std::string request = R"({"version":1,"requestId":1,"type":"ShowSettings","payload":{}})";
    std::string response;
    IpcServer::SendRequest(IpcServer::GetDefaultPipeName(), request, response, 1500);
    return false;
}

bool LauncherApp::Initialize(HINSTANCE hInstance, const CommandLineOptions& options) {
    hInstance_ = hInstance;
    cmdOptions_ = options;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wakeEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!wakeEvent_.IsValid()) {
        return false;
    }

    diagnostics_ = std::make_unique<DiagnosticsManager>(clock_);

    configStore_ = std::make_unique<ConfigStore>(ConfigStore::GetDefaultStorageDirectory());
    ValidationResult loadReport{};
    configStore_->LoadOrRecover(&loadReport);
    for (const auto& error : loadReport.errors) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Warning, "Config", "Recovered: " + error);
    }
    for (const auto& warning : loadReport.warnings) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Config", warning);
    }

    const auto initialCfg = configStore_->GetSnapshot();
    paused_.store(!initialCfg->preferences.enabled, std::memory_order_relaxed);
    diagnostics_->SetRedactUserPaths(initialCfg->preferences.redactPathsInDiagnostics);

    QuerySystemAccessibilityAndTheme();

    gestureMachine_ = std::make_unique<GestureStateMachine>(clock_);
    gestureMachine_->SetConfigSnapshotProvider([this]() { return configStore_->GetSnapshot(); });
    gestureMachine_->SetTargetAvailabilityCheck([](const ActionDefinition& action) {
        return ActionDispatcher::IsActionTargetAvailable(action);
    });

    animator_ = std::make_unique<RingAnimationController>(clock_);
    ApplyEffectiveMotionPreferences(initialCfg->preferences);

    if (!CreateOverlayWindow(hInstance)) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Error, "Lifecycle", "Failed to create overlay window.");
        return false;
    }

    accessibilityProvider_.Attach(new RadialOverlayAccessibilityProvider(overlayHwnd_));

    renderer_ = std::make_unique<D3D12OverlayRenderer>(*clock_, *diagnostics_);
    const uint32_t initialSizePx =
        static_cast<uint32_t>(DipToPhysicalPx(initialCfg->preferences.ringDiameterDip + 100.0f, currentDpi_)) + 2;
    if (!renderer_->Initialize(overlayHwnd_, initialSizePx, initialSizePx,
                               Utf8ToWide(initialCfg->preferences.preferredAdapterLuid))) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Error, "Graphics", "Renderer initialization failed.");
        return false;
    }

    renderer_->PrecacheConfigAssets(*initialCfg, currentDpi_);
    if (!renderer_->PrewarmTransparentFrame()) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Warning, "Graphics",
                                  "Initial transparent frame prewarm returned false.");
    }

    diagnostics_->SetGraphicsHardwareInfo(
        WideToUtf8(renderer_->SelectedAdapterName()),
        renderer_->SelectedAdapterLuid(),
        "D3D_FEATURE_LEVEL_12_0",
        "SM_6_0",
        renderer_->HasCompositorClockSupport() ? "CompositorClock" : "WaitableSwapChain",
        renderer_->GetAdapterDiagnostics());

    actionDispatcher_ = std::make_unique<ActionDispatcher>(clock_);
    actionDispatcher_->SetLaunchSettingsCallback(
        [this](std::string& outError) { return LaunchSettingsProcess(outError); });
    actionDispatcher_->SetCompletionCallback([this](const ActionExecutionResult& result) {
        const DiagnosticSeverity severity =
            (result.status == ActionExecutionStatus::Succeeded) ? DiagnosticSeverity::Info
                                                                : DiagnosticSeverity::Warning;
        std::string message =
            "Action '" + result.actionLabel + "' -> " + ActionExecutionStatusToString(result.status);
        if (result.win32Error != 0) {
            message += " (Win32 error " + std::to_string(result.win32Error) + ")";
        }
        if (!result.diagnosticMessage.empty()) {
            message += ": " + result.diagnosticMessage;
        }
        diagnostics_->RecordEvent(severity, "ActionDispatch", message);
    });
    actionDispatcher_->Start();

    if (!cmdOptions_.runBenchmarks && !cmdOptions_.captureScreenshot) {
        inputManager_ = std::make_unique<InputManager>(wakeEvent_.Get());
        ApplyInputConfiguration(initialCfg->preferences);
        if (!inputManager_->Start()) {
            diagnostics_->RecordEvent(DiagnosticSeverity::Error, "Input", "Failed to start input hook thread.");
            return false;
        }
        const InputHookStatus status = inputManager_->GetStatus();
        diagnostics_->SetInputStatus(status.keyboardHookInstalled, status.mouseHookInstalled,
                                     status.ctrlAltSpaceAvailable, status.reinstallCount);

        IpcHandlers handlers{};
        handlers.onGetConfig = [this]() { return HandleGetConfig(); };
        handlers.onApplyConfig = [this](const ConfigDocument& cfg, uint64_t& outRevision) {
            return HandleApplyConfig(cfg, outRevision);
        };
        handlers.onPreviewStart = [this](const ConfigDocument& cfg) { return HandlePreviewStart(cfg); };
        handlers.onPreviewUpdate = [this](const ConfigDocument& cfg) { return HandlePreviewUpdate(cfg); };
        handlers.onPreviewStop = [this]() { HandlePreviewStop(); };
        handlers.onGetDiagnostics = [this]() { return HandleGetDiagnostics(); };
        handlers.onExportDiagnostics = [this](const std::filesystem::path& path, bool includeRawSamples) {
            return HandleExportDiagnostics(path, includeRawSamples);
        };
        handlers.onResetInput = [this]() { return HandleResetInput(); };
        handlers.onSetPaused = [this](bool paused) { HandleSetPaused(paused); };
        handlers.onIsPaused = [this]() { return HandleIsPaused(); };
        handlers.onShowSettings = [this](std::string& outError) { return HandleShowSettings(outError); };

        ipcServer_ = std::make_unique<IpcServer>(std::move(handlers));
        if (!ipcServer_->Start()) {
            diagnostics_->RecordEvent(DiagnosticSeverity::Error, "IPC", "Failed to start named pipe server.");
            return false;
        }

        if (initialCfg->preferences.showTrayIcon) {
            RegisterTrayIcon();
        }
        ApplyStartupRegistration(initialCfg->preferences.launchAtStartup);
    }

    diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Lifecycle",
                              "Keychain initialized on adapter: " + WideToUtf8(renderer_->SelectedAdapterName()));
    return true;
}

void LauncherApp::Shutdown() {
    quitRequested_.store(true, std::memory_order_release);
    if (wakeEvent_.IsValid()) {
        SetEvent(wakeEvent_.Get());
    }

    if (ipcServer_) {
        ipcServer_->Stop();
        ipcServer_.reset();
    }
    if (inputManager_) {
        inputManager_->Stop();
        inputManager_.reset();
    }
    if (gestureMachine_) {
        gestureMachine_->ResetAllState();
    }
    lastGestureState_ = GestureState::Idle;
    if (actionDispatcher_) {
        actionDispatcher_->Stop();
        actionDispatcher_.reset();
    }

    RemoveTrayIcon();

    if (overlayHwnd_) {
        if (wtsRegistered_) {
            WTSUnRegisterSessionNotification(overlayHwnd_);
            wtsRegistered_ = false;
        }
        DestroyWindow(overlayHwnd_);
        overlayHwnd_ = nullptr;
    }

    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
    }
    accessibilityProvider_.Reset();
}

int LauncherApp::Run() {
    if (cmdOptions_.captureScreenshot) {
        return RunScreenshotCapture();
    }
    if (cmdOptions_.runBenchmarks) {
        return RunBenchmarkSuite();
    }

    while (!quitRequested_.load(std::memory_order_acquire)) {
        ProcessWakeAndInputEvents();
        if (quitRequested_.load(std::memory_order_acquire)) {
            break;
        }

        const GestureState state = gestureMachine_->GetState();
        const bool gestureEngaged =
            (state == GestureState::Opening || state == GestureState::Active || state == GestureState::Closing);
        const bool animating = overlayWindowVisible_ && !animator_->IsSettled();
        bool previewVisible = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            previewVisible = previewActive_;
        }

        HANDLE waitHandles[1] = {wakeEvent_.Get()};
        if (animating || (previewVisible && !animator_->IsSettled())) {
            renderer_->WaitForNextFrameOrWake(wakeEvent_.Get(), 20);
            ProcessWakeAndInputEvents();
            RenderIfNecessary();
        } else if (gestureEngaged) {
            MsgWaitForMultipleObjectsEx(1, waitHandles, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            ProcessWakeAndInputEvents();
            RenderIfNecessary();
        } else if (previewVisible) {
            MsgWaitForMultipleObjectsEx(1, waitHandles, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            ProcessWakeAndInputEvents();
            RenderIfNecessary();
        } else {
            MsgWaitForMultipleObjectsEx(1, waitHandles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            ProcessWakeAndInputEvents();
            RenderIfNecessary();
        }
    }
    return 0;
}

bool LauncherApp::CreateOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &LauncherApp::OverlayWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kOverlayWindowClassName;
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    overlayHwnd_ = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayWindowClassName,
        L"Keychain Radial Overlay",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 480,
        nullptr, nullptr, hInstance, this);
    if (!overlayHwnd_) {
        return false;
    }

    currentDpi_ = GetDpiForWindow(overlayHwnd_);
    if (currentDpi_ == 0) {
        currentDpi_ = 96;
    }

    wtsRegistered_ = (WTSRegisterSessionNotification(overlayHwnd_, NOTIFY_FOR_THIS_SESSION) == TRUE);
    return true;
}

LRESULT CALLBACK LauncherApp::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    LauncherApp* self = reinterpret_cast<LauncherApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->HandleWindowMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT LauncherApp::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST: {
            if (!overlayWindowVisible_) {
                return HTTRANSPARENT;
            }
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const float dx = static_cast<float>(pt.x) - currentLayout_.anchorInOverlayPx.x;
            const float dy = static_cast<float>(pt.y) - currentLayout_.anchorInOverlayPx.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            return (distance <= currentLayout_.outerRadiusPx + 12.0f) ? HTCLIENT : HTTRANSPARENT;
        }

        case WM_DPICHANGED: {
            currentDpi_ = HIWORD(wParam);
            if (overlayWindowVisible_) {
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_POWERBROADCAST: {
            if (msg == WM_POWERBROADCAST && wParam != PBT_APMSUSPEND) {
                break;
            }
            ApplyGestureStep(gestureMachine_->OnSessionOrDesktopLost(), clock_->NowTicks());
            HideOverlayWindow();
            break;
        }

        case WM_WTSSESSION_CHANGE: {
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF || wParam == WTS_CONSOLE_DISCONNECT) {
                ApplyGestureStep(gestureMachine_->OnSessionOrDesktopLost(), clock_->NowTicks());
                HideOverlayWindow();
            }
            break;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            QuerySystemAccessibilityAndTheme();
            break;

        case WM_GETOBJECT: {
            if (lParam == static_cast<LPARAM>(UiaRootObjectId) && accessibilityProvider_) {
                return accessibilityProvider_->HandleWmGetObject(wParam, lParam);
            }
            break;
        }

        case WM_KEYCHAIN_TRAY_CALLBACK: {
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                ShowTrayContextMenu();
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                std::string err;
                LaunchSettingsProcess(err);
            }
            break;
        }

        case WM_COMMAND: {
            const int cmd = LOWORD(wParam);
            if (cmd == ID_TRAY_OPEN_SETTINGS) {
                std::string err;
                LaunchSettingsProcess(err);
            } else if (cmd == ID_TRAY_TOGGLE_PAUSE) {
                HandleSetPaused(!paused_.load(std::memory_order_relaxed));
            } else if (cmd == ID_TRAY_RESET_INPUT) {
                HandleResetInput();
            } else if (cmd == ID_TRAY_EXIT) {
                quitRequested_.store(true, std::memory_order_release);
                SetEvent(wakeEvent_.Get());
            } else if (cmd >= ID_TRAY_PROFILE_BASE && cmd < ID_TRAY_PROFILE_BASE + kMaxTrayProfiles) {
                const size_t index = static_cast<size_t>(cmd - ID_TRAY_PROFILE_BASE);
                ConfigDocument updated = *configStore_->GetSnapshot();
                if (index < updated.profiles.size()) {
                    updated.activeProfileId = updated.profiles[index].id;
                    uint64_t revision = 0;
                    HandleApplyConfig(updated, revision);
                }
            }
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
                const auto cfg = configStore_->GetSnapshot();
                trayIconAdded_ = false;
                if (cfg->preferences.showTrayIcon) {
                    RegisterTrayIcon();
                }
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool LauncherApp::RegisterTrayIcon() {
    if (!overlayHwnd_ || trayIconAdded_) {
        return trayIconAdded_;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = overlayHwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_KEYCHAIN_TRAY_CALLBACK;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Keychain 1.0 Radial Launcher");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        trayIconAdded_ = true;
    }
    return trayIconAdded_;
}

void LauncherApp::RemoveTrayIcon() {
    if (!overlayHwnd_ || !trayIconAdded_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = overlayHwnd_;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayIconAdded_ = false;
}

void LauncherApp::ShowTrayContextMenu() {
    if (!overlayHwnd_) {
        return;
    }
    const auto cfg = configStore_->GetSnapshot();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_SETTINGS, L"Open Keychain &Settings...");

    HMENU profileMenu = nullptr;
    if (!cfg->profiles.empty()) {
        profileMenu = CreatePopupMenu();
        int count = 0;
        for (const auto& profile : cfg->profiles) {
            if (count >= kMaxTrayProfiles) {
                break;
            }
            const UINT flags = MF_STRING | ((profile.id == cfg->activeProfileId) ? MF_CHECKED : 0u);
            AppendMenuW(profileMenu, flags, ID_TRAY_PROFILE_BASE + count, Utf8ToWide(profile.name).c_str());
            ++count;
        }
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(profileMenu), L"Active &Profile");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (paused_.load(std::memory_order_relaxed) ? MF_CHECKED : 0u),
                ID_TRAY_TOGGLE_PAUSE, L"&Pause Hotring Input");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RESET_INPUT, L"Reset && Reinstall Input &Hooks");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"E&xit Keychain");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(overlayHwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, overlayHwnd_, nullptr);
    PostMessageW(overlayHwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (profileMenu) {
        DestroyMenu(profileMenu);
    }
}

bool LauncherApp::LaunchSettingsProcess(std::string& outError) {
    outError.clear();

    const std::filesystem::path exeDir = std::filesystem::path(GetSelfModuleDirectoryPath());
    const std::filesystem::path candidates[] = {
        exeDir / L"Keychain.Settings.exe",
        exeDir / L"settings" / L"Keychain.Settings.exe",
        exeDir.parent_path() / L"Keychain.Settings.exe",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            continue;
        }
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC;
        sei.lpVerb = L"open";
        sei.lpFile = candidate.c_str();
        sei.lpDirectory = candidate.parent_path().c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Settings", "Launched Keychain.Settings.exe");
            return true;
        }
    }

    outError = "Keychain.Settings.exe was not found next to Keychain.exe";
    diagnostics_->RecordEvent(DiagnosticSeverity::Warning, "Settings", outError);
    return false;
}

void LauncherApp::ApplyStartupRegistration(bool enable) {
    HKEY runKey = nullptr;
    const LONG openResult =
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &runKey, nullptr);
    if (openResult != ERROR_SUCCESS) {
        return;
    }

    if (enable) {
        wchar_t selfPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        const std::wstring quoted = L"\"" + std::wstring(selfPath) + L"\"";
        RegSetValueExW(runKey, L"Keychain", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(runKey, L"Keychain");
    }
    RegCloseKey(runKey);
}

void LauncherApp::ApplyEffectiveMotionPreferences(const Preferences& prefs) {
    bool reducedMotion = systemReducedMotion_;
    if (prefs.reducedMotionOverride == "On") {
        reducedMotion = true;
    } else if (prefs.reducedMotionOverride == "Off") {
        reducedMotion = false;
    }
    animator_->SetMotionPreferences(prefs.motionPreset, prefs.animationSpeedMultiplier, reducedMotion);
}

void LauncherApp::ApplyInputConfiguration(const Preferences& prefs) {
    inputManager_->SetConfigurationMode(
        prefs.enabled,
        prefs.activationMode == "WinCaps",
        prefs.activationMode == "CtrlAltSpace",
        prefs.interactionMode == "Toggle");
}

void LauncherApp::ProcessWakeAndInputEvents() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quitRequested_.store(true, std::memory_order_release);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (settingsLaunchRequested_.exchange(false, std::memory_order_acq_rel)) {
        std::string err;
        LaunchSettingsProcess(err);
    }

    if (inputResetRequested_.exchange(false, std::memory_order_acq_rel)) {
        gestureMachine_->ResetAllState();
        lastGestureState_ = GestureState::Idle;
        if (inputManager_) {
            inputManager_->ResetAndReinstallHooks();
            const InputHookStatus status = inputManager_->GetStatus();
            diagnostics_->SetInputStatus(status.keyboardHookInstalled, status.mouseHookInstalled,
                                         status.ctrlAltSpaceAvailable, status.reinstallCount);
        }
        diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Input", "Reset and reinstalled low-level input hooks.");
    }

    if (configUpdatedFlag_.exchange(false, std::memory_order_acq_rel)) {
        const auto cfg = configStore_->GetSnapshot();
        ApplyEffectiveMotionPreferences(cfg->preferences);
        if (inputManager_) {
            ApplyInputConfiguration(cfg->preferences);
        }
        renderer_->PrecacheConfigAssets(*cfg, currentDpi_);
        ApplyStartupRegistration(cfg->preferences.launchAtStartup);
        if (cfg->preferences.showTrayIcon) {
            RegisterTrayIcon();
        } else {
            RemoveTrayIcon();
        }
        diagnostics_->SetRedactUserPaths(cfg->preferences.redactPathsInDiagnostics);
        paused_.store(!cfg->preferences.enabled, std::memory_order_relaxed);
        HideOverlayWindow();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (previewActive_ && (clock_->NowMicroseconds() - previewStartTimeUs_) > kPreviewTimeoutUs) {
            previewActive_ = false;
        }
    }

    if (inputManager_) {
        InputQueueEvent ev{};
        while (inputManager_->PopEvent(ev)) {
            const bool suspended = paused_.load(std::memory_order_relaxed);
            gestureMachine_->SetSuspended(suspended);
            if (suspended) {
                continue;
            }

            const HMONITOR hMonitor =
                MonitorFromPoint(POINT{ev.cursorScreenPx.x, ev.cursorScreenPx.y}, MONITOR_DEFAULTTONEAREST);
            const RectI workArea = MonitorWorkAreaFromMonitor(hMonitor);
            const uint32_t dpi = MonitorDpiFromMonitor(hMonitor, currentDpi_);

            GestureStepResult res{};
            const HWND foreground = GetForegroundWindow();
            switch (ev.type) {
                case InputEventType::KeyEvent:
                    res = gestureMachine_->OnKeyEvent(ev.vkCode, ev.isDown, ev.isRepeat,
                                                      ev.capsToggleStateBeforeEvent, ev.cursorScreenPx,
                                                      workArea, dpi, foreground);
                    break;
                case InputEventType::HotKeyChord:
                    res = gestureMachine_->OnHotKeyChordTriggered(ev.cursorScreenPx, workArea, dpi, foreground);
                    break;
                case InputEventType::PointerMove:
                    res = gestureMachine_->OnPointerMove(ev.cursorScreenPx);
                    break;
                case InputEventType::MouseButton:
                    res = gestureMachine_->OnMouseButton(ev.isRightMouseButton, ev.isDown, ev.cursorScreenPx);
                    break;
                case InputEventType::QueueOverflow:
                    res = gestureMachine_->OnQueueOverflow();
                    diagnostics_->RecordEvent(DiagnosticSeverity::Warning, "InputQueue",
                                              "Input queue overflowed; active gesture safely cancelled.");
                    break;
                default:
                    break;
            }
            ApplyGestureStep(res, ev.receiptQpc != 0 ? ev.receiptQpc : clock_->NowTicks());
        }
    }

    if (gestureMachine_) {
        const GestureStepResult res = gestureMachine_->AdvanceTime(clock_->NowTicks(), GetForegroundWindow());
        ApplyGestureStep(res, clock_->NowTicks());
    }
}

void LauncherApp::ApplyGestureStep(const GestureStepResult& res, int64_t eventTicks) {
    if (res.emitStartMenuMask) {
        InputManager::EmitStartMenuMaskSequence();
    }

    const GestureState state = gestureMachine_->GetState();
    if (res.stateChanged) {
        if (inputManager_) {
            inputManager_->NotifyGestureActiveState(
                state == GestureState::Opening || state == GestureState::Active);
        }
        HandleGestureStateChangedTo(state, eventTicks);
    }

    if (res.navigatedChildRing || res.returnedToRootRing) {
        currentLayout_ = gestureMachine_->GetCurrentLayout();
        const auto snapshot = gestureMachine_->GetActiveConfigSnapshot();
        if (snapshot) {
            if (const RadialRing* ring = gestureMachine_->GetActiveRing()) {
                RefreshSectorAvailability(*snapshot, ring->id);
            }
        }
        animator_->OnSelectionChanged(gestureMachine_->GetSelectedSector(), currentLayout_);
    }

    if (res.selectionChanged) {
        const int selected = gestureMachine_->GetSelectedSector();
        animator_->OnSelectionChanged(selected, currentLayout_);

        if (accessibilityProvider_) {
            std::wstring label = L"None";
            bool available = true;
            const auto snapshot = gestureMachine_->GetActiveConfigSnapshot();
            if (snapshot) {
                if (const RadialRing* ring = gestureMachine_->GetActiveRing()) {
                    if (selected >= 0 && static_cast<size_t>(selected) < ring->sectorActionIds.size()) {
                        if (const ActionDefinition* action =
                                snapshot->FindAction(ring->sectorActionIds[static_cast<size_t>(selected)])) {
                            label = Utf8ToWide(action->label);
                            available = (static_cast<size_t>(selected) < currentSectorAvailable_.size())
                                            ? currentSectorAvailable_[static_cast<size_t>(selected)]
                                            : true;
                        }
                    }
                }
            }
            accessibilityProvider_->NotifySelectionChanged(selected, label, available);
        }
    }

    bool isPreview = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        isPreview = previewActive_;
    }

    if (res.immediateCommit.has_value()) {
        if (!isPreview) {
            actionDispatcher_->EnqueueCommittedAction(*res.immediateCommit);
        }
        animator_->OnGestureStateChanged(GestureState::Closing, true,
                                         gestureMachine_->GetSelectedSector(),
                                         gestureMachine_->GetCurrentLayout());
    } else if (res.readyDeferredCommit.has_value()) {
        if (!isPreview) {
            actionDispatcher_->EnqueueCommittedAction(*res.readyDeferredCommit);
        }
        animator_->OnGestureStateChanged(GestureState::Closing, true, -1,
                                         gestureMachine_->GetCurrentLayout());
    } else if (res.cancelReason != CancelReason::None) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Gesture",
                                  std::string("Gesture cancelled: ") + CancelReasonToString(res.cancelReason));
        animator_->OnGestureStateChanged(GestureState::Closing, false, -1,
                                         gestureMachine_->GetCurrentLayout());
    }
}

void LauncherApp::HandleGestureStateChangedTo(GestureState newState, int64_t eventTicks) {
    const GestureState previous = lastGestureState_;
    lastGestureState_ = newState;

    if (newState == GestureState::Opening && previous == GestureState::Idle) {
        pendingActivationReceiptTicks_ = eventTicks;
        pendingActivationRecognizedTicks_ = clock_->NowTicks();
        pendingRenderWakeTicks_ = pendingActivationRecognizedTicks_;
        awaitingFirstFrameOfActivation_ = true;

        // A live gesture takes precedence over a settings preview.
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            previewActive_ = false;
        }

        if (!IsInteractiveDesktopActive() || IsForegroundExcluded(GetForegroundWindow())) {
            const GestureStepResult cancel = gestureMachine_->OnSessionOrDesktopLost();
            if (inputManager_) {
                inputManager_->NotifyGestureActiveState(false);
            }
            ApplyGestureStep(cancel, eventTicks);
            return;
        }

        currentLayout_ = gestureMachine_->GetCurrentLayout();
        const auto snapshot = gestureMachine_->GetActiveConfigSnapshot();
        if (snapshot) {
            if (const RadialRing* ring = gestureMachine_->GetActiveRing()) {
                RefreshSectorAvailability(*snapshot, ring->id);
            }
        }

        animator_->OnGestureStateChanged(GestureState::Opening, false, -1, currentLayout_);
        ShowOverlayAtLayout(currentLayout_);
    } else if (newState == GestureState::Idle && previous != GestureState::Idle) {
        HideOverlayWindow();
    }
}

void LauncherApp::RenderIfNecessary() {
    const int64_t nowTicks = clock_->NowTicks();

    bool isPreview = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        isPreview = previewActive_;
    }

    if (isPreview != prevPreviewActive_) {
        if (isPreview) {
            actionDispatcher_->SetPreviewMode(true);
            animator_->OnGestureStateChanged(GestureState::Opening, false, -1, currentLayout_);
        } else {
            actionDispatcher_->SetPreviewMode(false);
            animator_->OnGestureStateChanged(GestureState::Closing, false, -1, currentLayout_);
        }
        prevPreviewActive_ = isPreview;
    }

    if (isPreview && previewLayoutDirty_.exchange(false, std::memory_order_acq_rel)) {
        std::shared_ptr<const ConfigDocument> previewCfg;
        std::string ringId;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            previewCfg = previewConfig_;
            ringId = previewRingId_;
        }
        if (previewCfg) {
            const RectI workArea = MonitorWorkAreaFromMonitor(
                MonitorFromWindow(overlayHwnd_, MONITOR_DEFAULTTOPRIMARY));
            const Point2I anchor{(workArea.left + workArea.right) / 2, (workArea.top + workArea.bottom) / 2};
            uint32_t sectorCount = 6;
            if (const RadialRing* ring = previewCfg->FindRing(ringId)) {
                sectorCount = ring->sectorCount;
            }
            currentLayout_ = RadialLayout::Compute(
                anchor, workArea, currentDpi_, sectorCount,
                previewCfg->preferences.ringDiameterDip,
                previewCfg->preferences.deadZoneRadiusDip,
                previewCfg->preferences.sectorGapDip);
            RefreshSectorAvailability(*previewCfg, ringId);
            ApplyEffectiveMotionPreferences(previewCfg->preferences);
            renderer_->PrecacheConfigAssets(*previewCfg, currentDpi_);
        }
    }

    std::shared_ptr<const ConfigDocument> activeCfg;
    std::string activeRingId;
    int selectedSector = -1;
    float dwellProgress = 0.0f;
    bool isChildRing = false;

    if (isPreview) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeCfg = previewConfig_;
        activeRingId = previewRingId_;
    } else {
        const GestureState state = gestureMachine_->GetState();
        if (state == GestureState::Idle) {
            if (!animator_->GetCurrentVisualState().isVisible) {
                if (overlayWindowVisible_) {
                    HideOverlayWindow();
                }
                return;
            }
        }
        activeCfg = gestureMachine_->GetActiveConfigSnapshot();
        if (!activeCfg) {
            activeCfg = configStore_->GetSnapshot();
        }
        if (const RadialRing* ring = gestureMachine_->GetActiveRing()) {
            activeRingId = ring->id;
        }
        selectedSector = gestureMachine_->GetSelectedSector();
        dwellProgress = gestureMachine_->GetDwellProgress(nowTicks);
        isChildRing = gestureMachine_->IsInChildRing();
    }

    if (!activeCfg) {
        return;
    }
    if (activeRingId.empty()) {
        if (const Profile* profile = activeCfg->FindActiveProfile()) {
            activeRingId = profile->rootRingId;
        }
    }
    if (activeRingId.empty()) {
        return;
    }

    const RingVisualState visual = animator_->Update(nowTicks);
    if (!visual.isVisible && !isPreview) {
        if (overlayWindowVisible_) {
            HideOverlayWindow();
        }
        return;
    }

    if (!overlayWindowVisible_) {
        ShowOverlayAtLayout(currentLayout_);
    }

    if (renderer_->IsPermanentlyFailed()) {
        return;
    }

    OverlayRenderParams params{};
    params.layout = currentLayout_;
    params.visual = visual;
    params.config = activeCfg.get();
    params.activeRingId = activeRingId;
    params.selectedSector = selectedSector;
    params.childRingDwellProgress = dwellProgress;
    params.isChildRing = isChildRing;
    params.isPreviewMode = isPreview;
    params.systemHighContrast = systemHighContrast_;
    params.systemReducedTransparency = systemTransparencyDisabled_;
    params.systemLightMode = systemLightTheme_;
    params.sectorAvailable = currentSectorAvailable_;

    const FrameTimingResult frameRes = renderer_->RenderFrame(params);
    if (frameRes.rendered) {
        const int64_t submitTicks = clock_->NowTicks();
        double frameIntervalMs = 0.0;
        if (lastFrameTicks_ > 0) {
            frameIntervalMs = clock_->TicksToMilliseconds(submitTicks - lastFrameTicks_);
        }
        lastFrameTicks_ = submitTicks;

        diagnostics_->RecordFrameTiming(frameIndexCounter_++, submitTicks, frameRes.cpuWorkMs,
                                        frameRes.previousFrameGpuWorkMs, frameIntervalMs);

        if (awaitingFirstFrameOfActivation_) {
            awaitingFirstFrameOfActivation_ = false;
            diagnostics_->RecordActivationLatency(
                gestureMachine_->GetCurrentGenerationId(),
                pendingActivationReceiptTicks_,
                pendingActivationRecognizedTicks_,
                pendingRenderWakeTicks_,
                submitTicks);
        }
    }

    if (frameRes.deviceLost && renderer_->IsPermanentlyFailed()) {
        diagnostics_->RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "Device lost repeatedly; rendering disabled until restart. Actions remain functional.");
    }
}

void LauncherApp::ShowOverlayAtLayout(const RadialLayout& geom) {
    const int32_t width = geom.overlayBoundsPx.Width();
    const int32_t height = geom.overlayBoundsPx.Height();
    if (width <= 0 || height <= 0) {
        return;
    }

    SetWindowPos(overlayHwnd_, HWND_TOPMOST, geom.overlayBoundsPx.left, geom.overlayBoundsPx.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    renderer_->ResizeIfNeeded(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    if (!overlayWindowVisible_) {
        overlayWindowVisible_ = true;
        lastFrameTicks_ = 0;
        renderer_->SetCompositorClockBoost(true);
        if (accessibilityProvider_) {
            std::wstring title = L"Keychain Ring";
            if (gestureMachine_ && gestureMachine_->GetActiveRing()) {
                title = Utf8ToWide(gestureMachine_->GetActiveRing()->name);
            }
            accessibilityProvider_->NotifyOverlayOpened(title);
        }
    }
}

void LauncherApp::HideOverlayWindow() {
    if (!overlayWindowVisible_) {
        return;
    }
    overlayWindowVisible_ = false;
    renderer_->SetCompositorClockBoost(false);
    ShowWindow(overlayHwnd_, SW_HIDE);
    lastFrameTicks_ = 0;
    if (accessibilityProvider_) {
        accessibilityProvider_->NotifyOverlayClosed();
    }
}

void LauncherApp::RefreshSectorAvailability(const ConfigDocument& cfg, const std::string& ringId) {
    const RadialRing* ring = cfg.FindRing(ringId);
    if (!ring) {
        currentSectorAvailable_.clear();
        return;
    }

    currentSectorAvailable_.assign(ring->sectorActionIds.size(), false);
    for (size_t i = 0; i < ring->sectorActionIds.size(); ++i) {
        const ActionDefinition* action = cfg.FindAction(ring->sectorActionIds[i]);
        if (!action || action->type == ActionType::None || !action->enabled) {
            currentSectorAvailable_[i] = false;
            continue;
        }
        if (action->type == ActionType::OpenChildRing) {
            currentSectorAvailable_[i] = (cfg.FindRing(action->childRingId) != nullptr);
        } else {
            currentSectorAvailable_[i] = ActionDispatcher::IsActionTargetAvailable(*action);
        }
    }
}

bool LauncherApp::IsForegroundExcluded(HWND fgHwnd) const {
    const auto cfg = configStore_->GetSnapshot();
    if (cfg->preferences.foregroundExclusions.empty() || !fgHwnd) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(fgHwnd, &pid);
    if (!pid) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    bool excluded = false;
    wchar_t imagePath[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, imagePath, &size)) {
        const wchar_t* fileName = FileNameFromPath(imagePath);
        for (const auto& exclusion : cfg->preferences.foregroundExclusions) {
            if (_wcsicmp(fileName, Utf8ToWide(exclusion).c_str()) == 0) {
                excluded = true;
                break;
            }
        }
    }
    CloseHandle(process);
    return excluded;
}

bool LauncherApp::IsInteractiveDesktopActive() {
    HDESK inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!inputDesktop) {
        return false;
    }
    wchar_t name[64]{};
    const BOOL ok = GetUserObjectInformationW(inputDesktop, UOI_NAME, name, sizeof(name), nullptr);
    CloseDesktop(inputDesktop);
    return ok && (wcscmp(name, L"Default") == 0);
}

void LauncherApp::QuerySystemAccessibilityAndTheme() {
    HIGHCONTRAST hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        systemHighContrast_ = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }

    BOOL clientAnimation = FALSE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimation, 0)) {
        systemReducedMotion_ = (clientAnimation == FALSE);
    }

    {
        DWORD value = 1;
        DWORD size = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
            systemLightTheme_ = (value != 0);
        }
    }
    {
        DWORD value = 1;
        DWORD size = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
            systemTransparencyDisabled_ = (value == 0);
        }
    }
}

std::shared_ptr<const ConfigDocument> LauncherApp::HandleGetConfig() const {
    return configStore_->GetSnapshot();
}

ValidationResult LauncherApp::HandleApplyConfig(const ConfigDocument& newConfig, uint64_t& outRevision) {
    outRevision = 0;
    ValidationResult result = configStore_->SaveAtomic(newConfig);
    if (result.valid) {
        outRevision = configStore_->GetSnapshot()->revision;
        configUpdatedFlag_.store(true, std::memory_order_release);
        SetEvent(wakeEvent_.Get());
    }
    return result;
}

bool LauncherApp::HandlePreviewStart(const ConfigDocument& previewConfig) {
    ValidationResult validation = ConfigStore::ValidateConfig(previewConfig);
    if (!validation.valid) {
        return false;
    }

    std::string ringId;
    if (const Profile* profile = previewConfig.FindActiveProfile()) {
        ringId = profile->rootRingId;
    }
    if (ringId.empty() && !previewConfig.rings.empty()) {
        ringId = previewConfig.rings.front().id;
    }
    if (ringId.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        previewActive_ = true;
        previewStartTimeUs_ = clock_->NowMicroseconds();
        previewConfig_ = std::make_shared<const ConfigDocument>(previewConfig);
        previewRingId_ = ringId;
    }
    previewLayoutDirty_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_.Get());
    return true;
}

bool LauncherApp::HandlePreviewUpdate(const ConfigDocument& previewConfig) {
    ValidationResult validation = ConfigStore::ValidateConfig(previewConfig);
    if (!validation.valid) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!previewActive_) {
            return false;
        }
        previewConfig_ = std::make_shared<const ConfigDocument>(previewConfig);
        if (const Profile* profile = previewConfig_->FindActiveProfile()) {
            if (const RadialRing* ring = previewConfig_->FindRing(profile->rootRingId)) {
                previewRingId_ = ring->id;
            } else {
                previewRingId_ = profile->rootRingId;
            }
        }
    }
    previewLayoutDirty_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_.Get());
    return true;
}

void LauncherApp::HandlePreviewStop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        previewActive_ = false;
    }
    SetEvent(wakeEvent_.Get());
}

DiagnosticsSnapshot LauncherApp::HandleGetDiagnostics() {
    if (inputManager_) {
        const InputHookStatus status = inputManager_->GetStatus();
        diagnostics_->SetInputStatus(status.keyboardHookInstalled, status.mouseHookInstalled,
                                     status.ctrlAltSpaceAvailable, status.reinstallCount);
    }
    return diagnostics_->CaptureSnapshot();
}

bool LauncherApp::HandleExportDiagnostics(const std::filesystem::path& path, bool includeRawSamples) {
    if (!diagnostics_) {
        return false;
    }
    return diagnostics_->ExportDiagnosticsReport(path, includeRawSamples);
}

bool LauncherApp::HandleResetInput() {
    if (!inputManager_) {
        return false;
    }
    inputResetRequested_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_.Get());
    return true;
}

void LauncherApp::HandleSetPaused(bool paused) {
    paused_.store(paused, std::memory_order_release);
    if (paused) {
        gestureMachine_->ResetAllState();
        lastGestureState_ = GestureState::Idle;
        HideOverlayWindow();
    }
    SetEvent(wakeEvent_.Get());
}

bool LauncherApp::HandleIsPaused() const {
    return paused_.load(std::memory_order_acquire);
}

bool LauncherApp::HandleShowSettings(std::string& outError) {
    outError.clear();
    settingsLaunchRequested_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_.Get());
    return true;
}

int LauncherApp::RunScreenshotCapture() {
    ConfigDocument cfg = *configStore_->GetSnapshot();
    cfg.preferences.theme = cmdOptions_.screenshotLightTheme ? "Light" : "Dark";
    const auto configSnapshot = std::make_shared<const ConfigDocument>(std::move(cfg));

    std::string targetRingId;
    if (const Profile* profile = configSnapshot->FindActiveProfile()) {
        targetRingId = profile->rootRingId;
    }
    if (cmdOptions_.screenshotChildRing) {
        for (const auto& ring : configSnapshot->rings) {
            if (ring.isChildRing) {
                targetRingId = ring.id;
                break;
            }
        }
    }
    if (targetRingId.empty()) {
        return 1;
    }

    uint32_t sectorCount = 6;
    bool isChildRing = false;
    if (const RadialRing* ring = configSnapshot->FindRing(targetRingId)) {
        sectorCount = ring->sectorCount;
        isChildRing = ring->isChildRing;
    }

    const RectI workArea = MonitorWorkAreaFromMonitor(MonitorFromWindow(overlayHwnd_, MONITOR_DEFAULTTOPRIMARY));
    Point2I anchor{(workArea.left + workArea.right) / 2, (workArea.top + workArea.bottom) / 2};
    if (cmdOptions_.screenshotEdgeRight) {
        anchor.x = workArea.right - 24;
    }

    currentLayout_ = RadialLayout::Compute(
        anchor, workArea, currentDpi_, sectorCount,
        configSnapshot->preferences.ringDiameterDip,
        configSnapshot->preferences.deadZoneRadiusDip,
        configSnapshot->preferences.sectorGapDip);
    RefreshSectorAvailability(*configSnapshot, targetRingId);
    renderer_->PrecacheConfigAssets(*configSnapshot, currentDpi_);

    const int selected = std::clamp(cmdOptions_.screenshotSelectedSector, -1, 7);

    float dwellProgress = 0.0f;
    if (selected >= 0 && !isChildRing) {
        if (const RadialRing* ring = configSnapshot->FindRing(targetRingId)) {
            if (static_cast<size_t>(selected) < ring->sectorActionIds.size()) {
                if (const ActionDefinition* action =
                        configSnapshot->FindAction(ring->sectorActionIds[static_cast<size_t>(selected)])) {
                    if (action->type == ActionType::OpenChildRing) {
                        dwellProgress = 0.65f;
                    }
                }
            }
        }
    }

    ShowOverlayAtLayout(currentLayout_);
    animator_->OnGestureStateChanged(GestureState::Opening, false, -1, currentLayout_);
    if (selected >= 0) {
        animator_->OnSelectionChanged(selected, currentLayout_);
    }

    const int64_t stepTicks = clock_->Frequency() / 120; // ~8.3ms deterministic steps
    int64_t t = clock_->NowTicks();
    for (int i = 0; i < 40; ++i) {
        t += stepTicks;
        const RingVisualState visual = animator_->Update(t);

        OverlayRenderParams params{};
        params.layout = currentLayout_;
        params.visual = visual;
        params.config = configSnapshot.get();
        params.activeRingId = targetRingId;
        params.selectedSector = selected;
        params.childRingDwellProgress = dwellProgress;
        params.isChildRing = isChildRing;
        params.systemLightMode = cmdOptions_.screenshotLightTheme;
        params.sectorAvailable = currentSectorAvailable_;
        renderer_->RenderFrame(params);
    }

    renderer_->WaitForGpuIdle();
    const bool saved = renderer_->CaptureCurrentBackBufferToPng(cmdOptions_.screenshotOutputPath);
    HideOverlayWindow();

    diagnostics_->RecordEvent(saved ? DiagnosticSeverity::Info : DiagnosticSeverity::Error, "Screenshot",
                              saved ? "Saved overlay capture." : "Failed to save overlay capture.");

    if (!saved) {
        // Evidence instrumentation: leave a diagnostics trace when capture fails.
        const std::filesystem::path diagPath = cmdOptions_.screenshotOutputPath + L".diag.json";
        std::error_code ec;
        std::filesystem::create_directories(diagPath.parent_path(), ec);
        diagnostics_->ExportDiagnosticsReport(diagPath, true);
    }
    return saved ? 0 : 1;
}

int LauncherApp::RunBenchmarkSuite() {
    const auto cfg = configStore_->GetSnapshot();

    std::string rootRingId;
    uint32_t sectorCount = 6;
    if (const Profile* profile = cfg->FindActiveProfile()) {
        rootRingId = profile->rootRingId;
    }
    if (rootRingId.empty() && !cfg->rings.empty()) {
        rootRingId = cfg->rings.front().id;
    }
    if (const RadialRing* ring = cfg->FindRing(rootRingId)) {
        sectorCount = ring->sectorCount;
    }

    const RectI workArea = MonitorWorkAreaFromMonitor(MonitorFromWindow(overlayHwnd_, MONITOR_DEFAULTTOPRIMARY));
    const Point2I anchor{(workArea.left + workArea.right) / 2, (workArea.top + workArea.bottom) / 2};

    currentLayout_ = RadialLayout::Compute(
        anchor, workArea, currentDpi_, sectorCount,
        cfg->preferences.ringDiameterDip,
        cfg->preferences.deadZoneRadiusDip,
        cfg->preferences.sectorGapDip);
    RefreshSectorAvailability(*cfg, rootRingId);

    // Phase 1: hidden idle — zero submitted frames, near-zero CPU.
    FILETIME ftCreate{}, ftExit{}, ftKernelStart{}, ftUserStart{};
    GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernelStart, &ftUserStart);
    const int64_t idleStartTicks = clock_->NowTicks();

    const int idleMs = std::max(1, cmdOptions_.benchmarkIdleSeconds) * 1000;
    WaitForSingleObject(wakeEvent_.Get(), static_cast<DWORD>(idleMs));

    FILETIME ftKernelEnd{}, ftUserEnd{};
    GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernelEnd, &ftUserEnd);
    const double idleDurationSec =
        clock_->TicksToMilliseconds(clock_->NowTicks() - idleStartTicks) / 1000.0;
    auto to100ns = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const double idleCpuMs = static_cast<double>(
        (to100ns(ftKernelEnd) - to100ns(ftKernelStart)) +
        (to100ns(ftUserEnd) - to100ns(ftUserStart))) / 10000.0;
    const uint64_t idleSubmittedFrames = diagnostics_->CaptureSnapshot().totalSubmittedFrames;

    // Phase 2: 50 warm activations — hotkey-to-first-frame-submission.
    for (int i = 0; i < 50; ++i) {
        const int64_t hotkeyTicks = clock_->NowTicks();
        const HWND foreground = GetForegroundWindow();

        gestureMachine_->ResetAllState();
        lastGestureState_ = GestureState::Idle;
        gestureMachine_->OnKeyEvent(VK_LWIN, true, false, false, anchor, workArea, currentDpi_, foreground);
        gestureMachine_->OnKeyEvent(VK_CAPITAL, true, false, false, anchor, workArea, currentDpi_, foreground);
        const int64_t recognizedTicks = clock_->NowTicks();

        currentLayout_ = gestureMachine_->GetCurrentLayout();
        animator_->OnGestureStateChanged(GestureState::Opening, false, -1, currentLayout_);
        ShowOverlayAtLayout(currentLayout_);

        OverlayRenderParams params{};
        params.layout = currentLayout_;
        params.visual = animator_->Update(clock_->NowTicks());
        params.config = cfg.get();
        params.activeRingId = rootRingId;
        params.selectedSector = 0;
        params.sectorAvailable = currentSectorAvailable_;
        renderer_->RenderFrame(params);
        const int64_t submitTicks = clock_->NowTicks();

        diagnostics_->RecordActivationLatency(
            gestureMachine_->GetCurrentGenerationId(), hotkeyTicks, recognizedTicks, hotkeyTicks, submitTicks);

        HideOverlayWindow();
    }

    // Phase 3: sustained 5-second diagnostic animation at compositor cadence.
    gestureMachine_->ResetAllState();
    lastGestureState_ = GestureState::Idle;
    ShowOverlayAtLayout(currentLayout_);
    animator_->OnGestureStateChanged(GestureState::Opening, false, -1, currentLayout_);

    const int64_t animStartTicks = clock_->NowTicks();
    int64_t prevFrameTicks = animStartTicks;
    uint64_t frameCounter = 0;

    while (clock_->TicksToMilliseconds(clock_->NowTicks() - animStartTicks) < 5000.0) {
        renderer_->WaitForNextFrameOrWake(wakeEvent_.Get(), 16);
        const int64_t nowTicks = clock_->NowTicks();
        const int selected = static_cast<int>((frameCounter / 30) % sectorCount);
        if (frameCounter % 30 == 0) {
            animator_->OnSelectionChanged(selected, currentLayout_);
        }

        OverlayRenderParams params{};
        params.layout = currentLayout_;
        params.visual = animator_->Update(nowTicks);
        params.config = cfg.get();
        params.activeRingId = rootRingId;
        params.selectedSector = selected;
        params.sectorAvailable = currentSectorAvailable_;
        const FrameTimingResult frameRes = renderer_->RenderFrame(params);

        if (frameRes.rendered) {
            const int64_t submitTicks = clock_->NowTicks();
            const double intervalMs = clock_->TicksToMilliseconds(submitTicks - prevFrameTicks);
            prevFrameTicks = submitTicks;
            diagnostics_->RecordFrameTiming(frameCounter, submitTicks, frameRes.cpuWorkMs,
                                            frameRes.previousFrameGpuWorkMs, intervalMs);
        }
        ++frameCounter;
    }

    // Phase 4: settled-visible behavior — no frames should be submitted while settled.
    while (!animator_->IsSettled()) {
        animator_->Update(clock_->NowTicks());
    }
    {
        OverlayRenderParams params{};
        params.layout = currentLayout_;
        params.visual = animator_->GetCurrentVisualState();
        params.config = cfg.get();
        params.activeRingId = rootRingId;
        params.selectedSector = static_cast<int>((frameCounter / 30) % sectorCount);
        params.sectorAvailable = currentSectorAvailable_;
        renderer_->RenderFrame(params);
    }
    const uint64_t settledBefore = diagnostics_->CaptureSnapshot().totalSubmittedFrames;
    Sleep(2000); // Settled: nothing re-renders in this window because RunBenchmarkSuite owns the loop.
    const uint64_t settledAfter = diagnostics_->CaptureSnapshot().totalSubmittedFrames;
    const uint64_t settledExtraFrames = (settledAfter > settledBefore) ? (settledAfter - settledBefore) : 0;

    HideOverlayWindow();
    renderer_->WaitForGpuIdle();

    const auto snap = diagnostics_->CaptureSnapshot();
    nlohmann::json report;
    report["hardware"] = {
        {"adapterDescription", snap.selectedAdapterName},
        {"adapterLuid", snap.selectedAdapterLuid},
        {"featureLevel", snap.featureLevel},
        {"shaderModel", snap.shaderModel},
        {"compositorClockStatus", snap.compositorClockStatus},
        {"compositorClockAvailable", renderer_->HasCompositorClockSupport()},
        {"activeMonitorRefreshHz", snap.activeMonitorRefreshHz},
        {"activeMonitorDpi", snap.activeMonitorDpi},
    };

    nlohmann::json monitors = nlohmann::json::array();
    for (const auto& monitor : snap.monitors) {
        monitors.push_back({
            {"deviceName", monitor.deviceName},
            {"refreshRateHz", monitor.refreshRateHz},
            {"dpi", monitor.dpi},
            {"isPrimary", monitor.isPrimary},
        });
    }
    report["monitors"] = std::move(monitors);

    report["idleVerification"] = {
        {"durationSeconds", idleDurationSec},
        {"submittedFramesDuringHiddenIdle", idleSubmittedFrames},
        {"totalProcessCpuMsDuringIdle", idleCpuMs},
        {"extraFramesDuringSettledVisible2s", settledExtraFrames},
    };

    report["hotkeyToFirstSubmissionMs"] = {
        {"sampleCount", snap.hotkeyToFirstSubmitMs.sampleCount},
        {"p50", snap.hotkeyToFirstSubmitMs.p50},
        {"p95", snap.hotkeyToFirstSubmitMs.p95},
        {"p99", snap.hotkeyToFirstSubmitMs.p99},
        {"max", snap.hotkeyToFirstSubmitMs.maxVal},
    };
    report["activeFrameCpuMs"] = {
        {"sampleCount", snap.cpuFrameWorkMs.sampleCount},
        {"p50", snap.cpuFrameWorkMs.p50},
        {"p95", snap.cpuFrameWorkMs.p95},
        {"p99", snap.cpuFrameWorkMs.p99},
        {"max", snap.cpuFrameWorkMs.maxVal},
    };
    report["activeFrameGpuMs"] = {
        {"sampleCount", snap.gpuFrameWorkMs.sampleCount},
        {"p50", snap.gpuFrameWorkMs.p50},
        {"p95", snap.gpuFrameWorkMs.p95},
        {"p99", snap.gpuFrameWorkMs.p99},
        {"max", snap.gpuFrameWorkMs.maxVal},
    };
    report["frameIntervalMs"] = {
        {"sampleCount", snap.activeFrameIntervalMs.sampleCount},
        {"p50", snap.activeFrameIntervalMs.p50},
        {"p95", snap.activeFrameIntervalMs.p95},
        {"p99", snap.activeFrameIntervalMs.p99},
    };
    report["memoryAndHandles"] = {
        {"privateMiB", snap.privateBytesMib},
        {"workingSetMiB", snap.workingSetMib},
        {"handleCount", snap.handleCount},
        {"totalSubmittedFrames", snap.totalSubmittedFrames},
    };

    if (!cmdOptions_.benchmarkOutputPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(cmdOptions_.benchmarkOutputPath).parent_path(), ec);
        std::ofstream out(std::filesystem::path(cmdOptions_.benchmarkOutputPath),
                          std::ios::binary | std::ios::trunc);
        out << report.dump(2);
    }

    diagnostics_->RecordEvent(DiagnosticSeverity::Info, "Benchmark", "Benchmark suite completed.");
    return 0;
}

} // namespace keychain
