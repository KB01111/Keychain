#include "ipc/ipc_server.h"
#include <nlohmann/json.hpp>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>

namespace keychain {

using json = nlohmann::json;

std::wstring IpcServer::GetDefaultPipeName() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return L"\\\\.\\pipe\\Keychain.Ipc.v1." + std::to_wstring(sessionId);
}

namespace {

struct ScopedSecurityDescriptor {
    PSECURITY_DESCRIPTOR pSD  = nullptr;
    PACL                 pAcl = nullptr;
    std::vector<uint8_t> tokenUserBuf;
    std::vector<uint8_t> tokenGroupsBuf;

    ~ScopedSecurityDescriptor() {
        if (pAcl) LocalFree(pAcl);
        if (pSD)  LocalFree(pSD);
    }
};

// Restricts the named pipe to the current logon session SID (and current user SID)
bool BuildCurrentSessionSecurityDescriptor(ScopedSecurityDescriptor& outSec, SECURITY_ATTRIBUTES& outSa) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    UniqueHandle tokenGuard(hToken);

    DWORD userLen = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &userLen);
    if (userLen == 0) return false;
    outSec.tokenUserBuf.resize(userLen);
    if (!GetTokenInformation(hToken, TokenUser, outSec.tokenUserBuf.data(), userLen, &userLen)) {
        return false;
    }
    PSID userSid = reinterpret_cast<PTOKEN_USER>(outSec.tokenUserBuf.data())->User.Sid;

    PSID logonSid = nullptr;
    DWORD groupsLen = 0;
    GetTokenInformation(hToken, TokenGroups, nullptr, 0, &groupsLen);
    if (groupsLen > 0) {
        outSec.tokenGroupsBuf.resize(groupsLen);
        if (GetTokenInformation(hToken, TokenGroups, outSec.tokenGroupsBuf.data(), groupsLen, &groupsLen)) {
            auto* groups = reinterpret_cast<PTOKEN_GROUPS>(outSec.tokenGroupsBuf.data());
            for (DWORD i = 0; i < groups->GroupCount; ++i) {
                if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                    logonSid = groups->Groups[i].Sid;
                    break;
                }
            }
        }
    }

    EXPLICIT_ACCESSW ea[2]{};
    ULONG count = 0;

    // Allow current user SID
    ea[count].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE;
    ea[count].grfAccessMode        = SET_ACCESS;
    ea[count].grfInheritance       = NO_INHERITANCE;
    ea[count].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea[count].Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea[count].Trustee.ptstrName    = reinterpret_cast<LPWSTR>(userSid);
    ++count;

    // Allow current logon session SID if available
    if (logonSid) {
        ea[count].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE;
        ea[count].grfAccessMode        = SET_ACCESS;
        ea[count].grfInheritance       = NO_INHERITANCE;
        ea[count].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea[count].Trustee.TrusteeType  = TRUSTEE_IS_GROUP;
        ea[count].Trustee.ptstrName    = reinterpret_cast<LPWSTR>(logonSid);
        ++count;
    }

    if (SetEntriesInAclW(count, ea, nullptr, &outSec.pAcl) != ERROR_SUCCESS) {
        return false;
    }

    outSec.pSD = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!outSec.pSD) return false;

    if (!InitializeSecurityDescriptor(outSec.pSD, SECURITY_DESCRIPTOR_REVISION)) {
        return false;
    }
    if (!SetSecurityDescriptorDacl(outSec.pSD, TRUE, outSec.pAcl, FALSE)) {
        return false;
    }

    outSa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    outSa.lpSecurityDescriptor = outSec.pSD;
    outSa.bInheritHandle       = FALSE;
    return true;
}

bool ReadExactOverlapped(HANDLE hPipe, HANDLE hStopEvent, void* buffer, DWORD bytesToRead, DWORD timeoutMs) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    DWORD totalRead = 0;

    UniqueHandle ioEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ioEvent) return false;

    while (totalRead < bytesToRead) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent.Get();
        ResetEvent(ioEvent.Get());

        DWORD chunkRead = 0;
        BOOL ok = ReadFile(hPipe, ptr + totalRead, bytesToRead - totalRead, &chunkRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;
            }
            HANDLE waits[2] = { ioEvent.Get(), hStopEvent };
            DWORD waitCount = hStopEvent ? 2 : 1;
            DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &chunkRead, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &chunkRead, FALSE)) {
                return false;
            }
        }
        if (chunkRead == 0) {
            return false;
        }
        totalRead += chunkRead;
    }
    return true;
}

bool WriteExactOverlapped(HANDLE hPipe, HANDLE hStopEvent, const void* buffer, DWORD bytesToWrite, DWORD timeoutMs) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
    DWORD totalWritten = 0;

    UniqueHandle ioEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ioEvent) return false;

    while (totalWritten < bytesToWrite) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent.Get();
        ResetEvent(ioEvent.Get());

        DWORD chunkWritten = 0;
        BOOL ok = WriteFile(hPipe, ptr + totalWritten, bytesToWrite - totalWritten, &chunkWritten, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;
            }
            HANDLE waits[2] = { ioEvent.Get(), hStopEvent };
            DWORD waitCount = hStopEvent ? 2 : 1;
            DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &chunkWritten, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &chunkWritten, FALSE)) {
                return false;
            }
        }
        if (chunkWritten == 0) {
            return false;
        }
        totalWritten += chunkWritten;
    }
    return true;
}

} // namespace

IpcServer::IpcServer(IpcHandlers handlers, std::wstring pipeName)
    : handlers_(std::move(handlers))
    , pipeName_(std::move(pipeName)) {
    stopEvent_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

IpcServer::~IpcServer() {
    Stop();
}

bool IpcServer::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    ResetEvent(stopEvent_.Get());
    serverThread_ = std::thread(&IpcServer::ServerThreadMain, this);
    return true;
}

void IpcServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (stopEvent_) {
        SetEvent(stopEvent_.Get());
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void IpcServer::ServerThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"KeychainIpcServerThread");

    ScopedSecurityDescriptor secDesc;
    SECURITY_ATTRIBUTES sa{};
    const bool hasSec = BuildCurrentSessionSecurityDescriptor(secDesc, sa);

    UniqueHandle connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    while (running_.load(std::memory_order_acquire)) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4,
            65536,
            65536,
            kIpcTimeoutMs,
            hasSec ? &sa : nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(stopEvent_.Get(), 100);
            continue;
        }
        UniqueHandle pipeGuard(hPipe);

        OVERLAPPED ov{};
        ResetEvent(connectEvent.Get());
        ov.hEvent = connectEvent.Get();

        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = { connectEvent.Get(), stopEvent_.Get() };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0) {
                    CancelIoEx(hPipe, &ov);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
            }
        }

        if (connected && running_.load(std::memory_order_acquire)) {
            HandleClientConnection(hPipe);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
        }
    }
}

void IpcServer::HandleClientConnection(HANDLE hPipe) {
    // Support multiple sequential request/response exchanges over a single client session
    // until the client disconnects or times out.
    while (running_.load(std::memory_order_acquire)) {
        uint32_t msgLen = 0;
        if (!ReadExactOverlapped(hPipe, stopEvent_.Get(), &msgLen, sizeof(msgLen), kIpcTimeoutMs)) {
            break;
        }
        if (msgLen == 0 || msgLen > kMaxIpcMessageBytes) {
            break;
        }

        std::string reqBuf(static_cast<size_t>(msgLen), '\0');
        if (!ReadExactOverlapped(hPipe, stopEvent_.Get(), reqBuf.data(), msgLen, kIpcTimeoutMs)) {
            break;
        }

        std::string respBuf = ProcessRequestJson(reqBuf);
        if (respBuf.size() > kMaxIpcMessageBytes) {
            json errResp = {
                {"version", kIpcProtocolVersion},
                {"requestId", ""},
                {"type", "ErrorResponse"},
                {"ok", false},
                {"error", "Response exceeded 1 MiB limit."}
            };
            respBuf = errResp.dump();
        }

        uint32_t respLen = static_cast<uint32_t>(respBuf.size());
        if (!WriteExactOverlapped(hPipe, stopEvent_.Get(), &respLen, sizeof(respLen), kIpcTimeoutMs)) {
            break;
        }
        if (!WriteExactOverlapped(hPipe, stopEvent_.Get(), respBuf.data(), respLen, kIpcTimeoutMs)) {
            break;
        }
    }
}

std::string IpcServer::ProcessRequestJson(std::string_view requestJson) {
    json resp = {
        {"version", kIpcProtocolVersion},
        {"requestId", ""},
        {"type", "ErrorResponse"},
        {"ok", false},
        {"error", ""},
        {"errors", json::array()},
        {"warnings", json::array()},
        {"payload", json::object()}
    };

    if (requestJson.empty() || requestJson.size() > kMaxIpcMessageBytes) {
        resp["error"] = "Invalid IPC message size.";
        return resp.dump();
    }

    json req;
    try {
        req = json::parse(requestJson.begin(), requestJson.end());
    } catch (const std::exception& ex) {
        resp["error"] = std::string("Malformed IPC JSON: ") + ex.what();
        return resp.dump();
    }

    if (!req.is_object()) {
        resp["error"] = "IPC request must be a JSON object.";
        return resp.dump();
    }

    const int32_t version     = req.value("version", 0);
    const std::string reqId   = req.value("requestId", "");
    const std::string reqType = req.value("type", "");
    resp["requestId"]         = reqId;

    if (version != kIpcProtocolVersion) {
        resp["error"] = "Unsupported IPC protocol version: " + std::to_string(version);
        return resp.dump();
    }

    if (reqType.empty()) {
        resp["error"] = "Missing IPC request type.";
        return resp.dump();
    }

    resp["type"] = reqType + "Response";
    const json payload = req.contains("payload") && req["payload"].is_object()
        ? req["payload"]
        : json::object();

    if (reqType == "GetConfig") {
        if (!handlers_.onGetConfig) {
            resp["error"] = "GetConfig handler not available.";
            return resp.dump();
        }
        auto snap = handlers_.onGetConfig();
        if (!snap) {
            resp["error"] = "Active configuration snapshot is null.";
            return resp.dump();
        }
        resp["ok"]      = true;
        resp["payload"] = json::parse(ConfigStore::SerializeToJson(*snap, false));
        return resp.dump();
    }

    if (reqType == "ApplyConfig") {
        if (!handlers_.onApplyConfig) {
            resp["error"] = "ApplyConfig handler not available.";
            return resp.dump();
        }
        std::string configJsonStr = payload.contains("config")
            ? (payload["config"].is_string() ? payload["config"].get<std::string>() : payload["config"].dump())
            : payload.dump();

        ConfigDocument newDoc;
        ValidationResult val;
        if (!ConfigStore::DeserializeFromJson(configJsonStr, newDoc, val)) {
            resp["ok"]       = false;
            resp["error"]    = val.errors.empty() ? "Configuration validation failed." : val.errors.front();
            resp["errors"]   = val.errors;
            resp["warnings"] = val.warnings;
            return resp.dump();
        }

        uint64_t newRev = newDoc.revision;
        ValidationResult saveVal = handlers_.onApplyConfig(newDoc, newRev);
        resp["ok"]       = saveVal.valid;
        resp["errors"]   = saveVal.errors;
        resp["warnings"] = saveVal.warnings;
        if (!saveVal.valid) {
            resp["error"] = saveVal.errors.empty() ? "Failed to save configuration." : saveVal.errors.front();
        } else {
            resp["payload"] = {{"revision", newRev}, {"applied", true}};
        }
        return resp.dump();
    }

    if (reqType == "ImportConfig") {
        std::string configJsonStr = payload.contains("config")
            ? (payload["config"].is_string() ? payload["config"].get<std::string>() : payload["config"].dump())
            : payload.dump();

        ConfigDocument importedDoc;
        ValidationResult val;
        if (!ConfigStore::ImportConfigFromJson(configJsonStr, importedDoc, val)) {
            resp["ok"]       = false;
            resp["error"]    = val.errors.empty() ? "Import validation failed." : val.errors.front();
            resp["errors"]   = val.errors;
            resp["warnings"] = val.warnings;
            return resp.dump();
        }
        resp["ok"]       = true;
        resp["warnings"] = val.warnings;
        resp["payload"]  = json::parse(ConfigStore::SerializeToJson(importedDoc, false));
        return resp.dump();
    }

    if (reqType == "PreviewStart" || reqType == "PreviewUpdate") {
        std::string configJsonStr = payload.contains("config")
            ? (payload["config"].is_string() ? payload["config"].get<std::string>() : payload["config"].dump())
            : payload.dump();

        ConfigDocument previewDoc;
        ValidationResult val;
        if (configJsonStr.empty() || configJsonStr == "{}") {
            auto snap = handlers_.onGetConfig ? handlers_.onGetConfig() : nullptr;
            previewDoc = snap ? *snap : ConfigStore::CreateDefaultConfig();
        } else if (!ConfigStore::DeserializeFromJson(configJsonStr, previewDoc, val)) {
            resp["ok"]     = false;
            resp["error"]  = val.errors.empty() ? "Invalid preview configuration." : val.errors.front();
            resp["errors"] = val.errors;
            return resp.dump();
        }

        bool ok = false;
        if (reqType == "PreviewStart" && handlers_.onPreviewStart) {
            ok = handlers_.onPreviewStart(previewDoc);
        } else if (reqType == "PreviewUpdate" && handlers_.onPreviewUpdate) {
            ok = handlers_.onPreviewUpdate(previewDoc);
        }
        resp["ok"]      = ok;
        resp["payload"] = {{"previewActive", ok}};
        if (!ok) resp["error"] = "Failed to start/update DX12 preview.";
        return resp.dump();
    }

    if (reqType == "PreviewStop") {
        if (handlers_.onPreviewStop) {
            handlers_.onPreviewStop();
        }
        resp["ok"]      = true;
        resp["payload"] = {{"previewActive", false}};
        return resp.dump();
    }

    if (reqType == "GetDiagnostics") {
        if (!handlers_.onGetDiagnostics) {
            resp["error"] = "GetDiagnostics handler not available.";
            return resp.dump();
        }
        DiagnosticsSnapshot snap = handlers_.onGetDiagnostics();
        DiagnosticsManager serializer;
        resp["ok"]      = true;
        resp["payload"] = json::parse(serializer.SerializeSnapshotToJson(snap, false));
        if (handlers_.onIsPaused) {
            resp["payload"]["paused"] = handlers_.onIsPaused();
        }
        return resp.dump();
    }

    if (reqType == "ExportDiagnostics") {
        if (!handlers_.onExportDiagnostics) {
            resp["error"] = "ExportDiagnostics handler not available.";
            return resp.dump();
        }
        std::string exportPathUtf8 = payload.value("path", "");
        bool includeRaw            = payload.value("includeRawSamples", true);
        if (exportPathUtf8.empty()) {
            resp["error"] = "Export path is required.";
            return resp.dump();
        }
        bool ok = handlers_.onExportDiagnostics(std::filesystem::path(Utf8ToWide(exportPathUtf8)), includeRaw);
        resp["ok"] = ok;
        if (!ok) resp["error"] = "Failed to write diagnostics export file.";
        return resp.dump();
    }

    if (reqType == "ResetInput") {
        bool ok = handlers_.onResetInput ? handlers_.onResetInput() : false;
        resp["ok"]      = ok;
        resp["payload"] = {{"hooksReinstalled", ok}};
        if (!ok) resp["error"] = "Failed to reinstall low-level input hooks.";
        return resp.dump();
    }

    if (reqType == "Pause") {
        if (handlers_.onSetPaused) {
            handlers_.onSetPaused(true);
        }
        resp["ok"]      = true;
        resp["payload"] = {{"paused", true}};
        return resp.dump();
    }

    if (reqType == "Resume") {
        if (handlers_.onSetPaused) {
            handlers_.onSetPaused(false);
        }
        resp["ok"]      = true;
        resp["payload"] = {{"paused", false}};
        return resp.dump();
    }

    if (reqType == "ShowSettings") {
        std::string err;
        bool ok = handlers_.onShowSettings ? handlers_.onShowSettings(err) : false;
        resp["ok"]      = ok;
        resp["payload"] = {{"launched", ok}};
        if (!ok) resp["error"] = err.empty() ? "Failed to open Keychain Settings." : err;
        return resp.dump();
    }

    resp["error"] = "Unknown IPC request type: " + reqType;
    return resp.dump();
}

bool IpcServer::SendRequest(
    const std::wstring& pipeName,
    std::string_view requestJson,
    std::string& outResponseJson,
    DWORD timeoutMs
) {
    if (requestJson.empty() || requestJson.size() > kMaxIpcMessageBytes) {
        return false;
    }

    // Deadline-bounded connect: the server recreates pipe instances between
    // client connections, so a single WaitNamedPipeW probe can land in the
    // gap and spuriously fail. Retry until the overall timeout.
    const ULONGLONG connectDeadline = GetTickCount64() + (timeoutMs ? timeoutMs : 1);
    UniqueHandle pipeGuard;
    for (;;) {
        HANDLE hPipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (hPipe != INVALID_HANDLE_VALUE) {
            pipeGuard.Reset(hPipe);
            break;
        }
        const DWORD connectErr = GetLastError();
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick >= connectDeadline) {
            return false;
        }
        const DWORD remainingMs = static_cast<DWORD>(connectDeadline - nowTick);
        if (connectErr == ERROR_PIPE_BUSY) {
            // Transient: an instance exists but is not listening yet. Wait for it,
            // but do not abort if the wait itself misses — the server may be
            // recreating the instance at this exact moment.
            WaitNamedPipeW(pipeName.c_str(), remainingMs);
        } else if (connectErr == ERROR_FILE_NOT_FOUND || connectErr == ERROR_PATH_NOT_FOUND ||
                   connectErr == ERROR_NO_DATA || connectErr == ERROR_BROKEN_PIPE ||
                   connectErr == ERROR_PIPE_NOT_CONNECTED || connectErr == ERROR_SEM_TIMEOUT) {
            // The server recreates pipe instances between client connections;
            // a client can momentarily observe a disconnected/absent instance.
            Sleep(2);
        } else {
            return false;
        }
    }

    HANDLE hPipe = pipeGuard.Get();
    const uint32_t reqLen = static_cast<uint32_t>(requestJson.size());
    if (!WriteExactOverlapped(hPipe, nullptr, &reqLen, sizeof(reqLen), timeoutMs)) {
        return false;
    }
    if (!WriteExactOverlapped(hPipe, nullptr, requestJson.data(), reqLen, timeoutMs)) {
        return false;
    }

    uint32_t respLen = 0;
    if (!ReadExactOverlapped(hPipe, nullptr, &respLen, sizeof(respLen), timeoutMs)) {
        return false;
    }
    if (respLen == 0 || respLen > kMaxIpcMessageBytes) {
        return false;
    }

    outResponseJson.resize(static_cast<size_t>(respLen));
    if (!ReadExactOverlapped(hPipe, nullptr, outResponseJson.data(), respLen, timeoutMs)) {
        return false;
    }
    return true;
}

} // namespace keychain
