#include "app/launcher_app.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hr);

    keychain::CommandLineOptions options{};
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--benchmark" && (i + 1) < argc) {
                options.runBenchmarks = true;
                options.benchmarkOutputPath = argv[++i];
            } else if (arg == L"--idle-seconds" && (i + 1) < argc) {
                options.benchmarkIdleSeconds = _wtoi(argv[++i]);
            } else if (arg == L"--self-test-render" && (i + 1) < argc) {
                options.captureScreenshot = true;
                options.screenshotOutputPath = argv[++i];
            } else if (arg == L"--selected-sector" && (i + 1) < argc) {
                options.screenshotSelectedSector = _wtoi(argv[++i]);
            } else if (arg == L"--child-ring") {
                options.screenshotChildRing = true;
            } else if (arg == L"--light-theme") {
                options.screenshotLightTheme = true;
            } else if (arg == L"--edge-right") {
                options.screenshotEdgeRight = true;
            }
        }
        LocalFree(argv);
    }

    keychain::UniqueHandle instanceMutex;
    if (!options.runBenchmarks && !options.captureScreenshot) {
        if (!keychain::LauncherApp::AcquireSingleInstanceOrNotifyExisting(instanceMutex)) {
            if (comInitialized) {
                CoUninitialize();
            }
            return 0;
        }
    }

    int exitCode = 1;
    {
        keychain::LauncherApp app;
        if (app.Initialize(hInstance, options)) {
            exitCode = app.Run();
        } else {
            // Evidence instrumentation: export a diagnostics report when startup fails.
            std::error_code ec;
            const std::filesystem::path diagDir = std::filesystem::temp_directory_path(ec) / L"Keychain";
            std::filesystem::create_directories(diagDir, ec);
            app.HandleExportDiagnostics(diagDir / L"keychain-init-diag.json", true);
        }
    }

    if (comInitialized) {
        CoUninitialize();
    }
    return exitCode;
}
