#pragma once

#include "actions/action_dispatcher.h"
#include "animation/spring_animator.h"
#include "app/accessibility_provider.h"
#include "common/types.h"
#include "config/config_schema.h"
#include "diagnostics/diagnostics_manager.h"
#include "gesture/gesture_state_machine.h"
#include "graphics/d3d12_overlay_renderer.h"
#include "input/input_manager.h"
#include "ipc/ipc_server.h"
#include "layout/radial_layout.h"

#include <Windows.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace keychain {

struct CommandLineOptions {
    bool runBenchmarks = false;
    int benchmarkIdleSeconds = 60;
    std::wstring benchmarkOutputPath;

    bool captureScreenshot = false;
    std::wstring screenshotOutputPath;
    int screenshotSelectedSector = 1;
    bool screenshotChildRing = false;
    bool screenshotLightTheme = false;
    bool screenshotEdgeRight = false;
};

class LauncherApp final {
public:
    LauncherApp();
    ~LauncherApp();

    LauncherApp(const LauncherApp&) = delete;
    LauncherApp& operator=(const LauncherApp&) = delete;

    // Checks the single-instance mutex; if already running, notifies the primary
    // instance to open its Settings window and returns false.
    static bool AcquireSingleInstanceOrNotifyExisting(UniqueHandle& outMutex);

    bool Initialize(HINSTANCE hInstance, const CommandLineOptions& options = {});
    int Run();
    void Shutdown();

    // IPC handlers (invoked on the IPC worker thread; all are thread-safe)
    std::shared_ptr<const ConfigDocument> HandleGetConfig() const;
    ValidationResult HandleApplyConfig(const ConfigDocument& newConfig, uint64_t& outRevision);
    bool HandlePreviewStart(const ConfigDocument& previewConfig);
    bool HandlePreviewUpdate(const ConfigDocument& previewConfig);
    void HandlePreviewStop();
    DiagnosticsSnapshot HandleGetDiagnostics();
    bool HandleExportDiagnostics(const std::filesystem::path& path, bool includeRawSamples);
    bool HandleResetInput();
    void HandleSetPaused(bool paused);
    bool HandleIsPaused() const;
    bool HandleShowSettings(std::string& outError);

    // Launches Keychain.Settings.exe (callable from any thread)
    bool LaunchSettingsProcess(std::string& outError);

private:
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateOverlayWindow(HINSTANCE hInstance);
    bool RegisterTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayContextMenu();
    void ApplyStartupRegistration(bool enable);

    void ApplyEffectiveMotionPreferences(const Preferences& prefs);
    void ApplyInputConfiguration(const Preferences& prefs);
    void ProcessWakeAndInputEvents();
    void ApplyGestureStep(const GestureStepResult& res, int64_t eventTicks);
    void HandleGestureStateChangedTo(GestureState newState, int64_t eventTicks);
    void RenderIfNecessary();
    void ShowOverlayAtLayout(const RadialLayout& geom);
    void HideOverlayWindow();
    void RefreshSectorAvailability(const ConfigDocument& cfg, const std::string& ringId);

    bool IsForegroundExcluded(HWND fgHwnd) const;
    static bool IsInteractiveDesktopActive();
    void QuerySystemAccessibilityAndTheme();

    int RunBenchmarkSuite();
    int RunScreenshotCapture();

    HINSTANCE hInstance_ = nullptr;
    CommandLineOptions cmdOptions_{};
    HWND overlayHwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    bool trayIconAdded_ = false;
    bool wtsRegistered_ = false;

    std::shared_ptr<SystemQpcClock> clock_ = std::make_shared<SystemQpcClock>();
    UniqueHandle wakeEvent_;

    std::unique_ptr<ConfigStore> configStore_;
    std::unique_ptr<DiagnosticsManager> diagnostics_;
    std::unique_ptr<GestureStateMachine> gestureMachine_;
    std::unique_ptr<RingAnimationController> animator_;
    std::unique_ptr<D3D12OverlayRenderer> renderer_;
    std::unique_ptr<InputManager> inputManager_;
    std::unique_ptr<ActionDispatcher> actionDispatcher_;
    std::unique_ptr<IpcServer> ipcServer_;
    Microsoft::WRL::ComPtr<RadialOverlayAccessibilityProvider> accessibilityProvider_;

    mutable std::mutex stateMutex_;
    std::atomic<bool> quitRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> settingsLaunchRequested_{false};
    std::atomic<bool> inputResetRequested_{false};
    std::atomic<bool> configUpdatedFlag_{false};
    std::atomic<bool> previewLayoutDirty_{false};

    bool previewActive_ = false;
    int64_t previewStartTimeUs_ = 0;
    std::shared_ptr<const ConfigDocument> previewConfig_;
    std::string previewRingId_;

    bool prevPreviewActive_ = false;
    bool overlayWindowVisible_ = false;
    GestureState lastGestureState_ = GestureState::Idle;
    uint64_t frameIndexCounter_ = 0;
    int64_t lastFrameTicks_ = 0;
    int64_t pendingActivationReceiptTicks_ = 0;
    int64_t pendingActivationRecognizedTicks_ = 0;
    int64_t pendingRenderWakeTicks_ = 0;
    bool awaitingFirstFrameOfActivation_ = false;

    RadialLayout currentLayout_{};
    std::vector<bool> currentSectorAvailable_;

    bool systemHighContrast_ = false;
    bool systemTransparencyDisabled_ = false;
    bool systemReducedMotion_ = false;
    bool systemLightTheme_ = false;
    uint32_t currentDpi_ = 96;
};

} // namespace keychain
