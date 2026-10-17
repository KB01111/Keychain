#pragma once

#include "common/types.h"
#include "config/config_schema.h"
#include "diagnostics/diagnostics_manager.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace keychain {

constexpr int32_t  kIpcProtocolVersion = 1;
constexpr uint32_t kMaxIpcMessageBytes = 1024 * 1024; // 1 MiB
constexpr DWORD    kIpcTimeoutMs       = 3000;

struct IpcHandlers {
    std::function<std::shared_ptr<const ConfigDocument>()>                     onGetConfig;
    std::function<ValidationResult(const ConfigDocument&, uint64_t& outRev)>   onApplyConfig;
    std::function<bool(const ConfigDocument&)>                                 onPreviewStart;
    std::function<bool(const ConfigDocument&)>                                 onPreviewUpdate;
    std::function<void()>                                                      onPreviewStop;
    std::function<DiagnosticsSnapshot()>                                       onGetDiagnostics;
    std::function<bool(const std::filesystem::path&, bool)>                    onExportDiagnostics;
    std::function<bool()>                                                      onResetInput;
    std::function<void(bool paused)>                                           onSetPaused;
    std::function<bool()>                                                      onIsPaused;
    std::function<bool(std::string& outErr)>                                   onShowSettings;
};

class IpcServer {
public:
    explicit IpcServer(IpcHandlers handlers, std::wstring pipeName = GetDefaultPipeName());
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool Start();
    void Stop();

    const std::wstring& GetPipeName() const noexcept { return pipeName_; }
    static std::wstring GetDefaultPipeName();

    // Processes a raw UTF-8 JSON request string and returns the UTF-8 JSON response string.
    std::string ProcessRequestJson(std::string_view requestJson);

    // Client helper for sending a length-prefixed request to a running server instance.
    static bool SendRequest(
        const std::wstring& pipeName,
        std::string_view requestJson,
        std::string& outResponseJson,
        DWORD timeoutMs = kIpcTimeoutMs
    );

private:
    void ServerThreadMain();
    void HandleClientConnection(HANDLE hPipe);

    IpcHandlers handlers_;
    std::wstring pipeName_;
    std::atomic<bool> running_{false};
    UniqueHandle stopEvent_;
    std::thread serverThread_;
};

} // namespace keychain
