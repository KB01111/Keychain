#include "actions/action_dispatcher.h"
#include "input/input_manager.h"
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <cctype>

namespace keychain {

const char* ActionExecutionStatusToString(ActionExecutionStatus status) noexcept {
    switch (status) {
        case ActionExecutionStatus::Succeeded:               return "Succeeded";
        case ActionExecutionStatus::PreviewSuppressed:       return "PreviewSuppressed";
        case ActionExecutionStatus::UnapprovedActionBlocked: return "UnapprovedActionBlocked";
        case ActionExecutionStatus::DisabledActionBlocked:   return "DisabledActionBlocked";
        case ActionExecutionStatus::TargetNotFound:          return "TargetNotFound";
        case ActionExecutionStatus::InvalidUrlScheme:        return "InvalidUrlScheme";
        case ActionExecutionStatus::ShellInterpreterBlocked: return "ShellInterpreterBlocked";
        case ActionExecutionStatus::ForegroundWindowChanged: return "ForegroundWindowChanged";
        case ActionExecutionStatus::UipiOrSendInputBlocked:  return "UipiOrSendInputBlocked";
        case ActionExecutionStatus::ShellExecuteFailed:      return "ShellExecuteFailed";
    }
    return "Unknown";
}

ActionDispatcher::ActionDispatcher(std::shared_ptr<IClock> clock)
    : clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = std::make_shared<SystemQpcClock>();
    }
    workWakeEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

ActionDispatcher::~ActionDispatcher() {
    Stop();
}

void ActionDispatcher::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    workerThread_ = std::thread(&ActionDispatcher::WorkerThreadMain, this);
}

void ActionDispatcher::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    AcquireSRWLockExclusive(&queueLock_);
    workQueue_.clear();
    ReleaseSRWLockExclusive(&queueLock_);

    if (workWakeEvent_) {
        SetEvent(workWakeEvent_.Get());
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void ActionDispatcher::SetCompletionCallback(CompletionCallback cb) {
    AcquireSRWLockExclusive(&callbackLock_);
    completionCallback_ = std::move(cb);
    ReleaseSRWLockExclusive(&callbackLock_);
}

void ActionDispatcher::SetLaunchSettingsCallback(LaunchSettingsCallback cb) {
    AcquireSRWLockExclusive(&callbackLock_);
    launchSettingsCallback_ = std::move(cb);
    ReleaseSRWLockExclusive(&callbackLock_);
}

void ActionDispatcher::EnqueueCommittedAction(CommittedAction committed) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    AcquireSRWLockExclusive(&queueLock_);
    if (workQueue_.size() < 64) {
        workQueue_.push_back(std::move(committed));
    }
    ReleaseSRWLockExclusive(&queueLock_);
    SetEvent(workWakeEvent_.Get());
}

void ActionDispatcher::WorkerThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"KeychainActionWorkerThread");
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    while (running_.load(std::memory_order_acquire)) {
        WaitForSingleObject(workWakeEvent_.Get(), INFINITE);

        while (running_.load(std::memory_order_acquire)) {
            std::optional<CommittedAction> nextItem;
            AcquireSRWLockExclusive(&queueLock_);
            if (!workQueue_.empty()) {
                nextItem = std::move(workQueue_.front());
                workQueue_.pop_front();
            }
            ReleaseSRWLockExclusive(&queueLock_);

            if (!nextItem.has_value()) {
                break;
            }

            ActionExecutionResult res = ExecuteSynchronously(*nextItem);

            CompletionCallback cbCopy;
            AcquireSRWLockShared(&callbackLock_);
            cbCopy = completionCallback_;
            ReleaseSRWLockShared(&callbackLock_);
            if (cbCopy) {
                cbCopy(res);
            }
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
}

std::wstring ActionDispatcher::ExpandPathStrings(std::string_view utf8Path) {
    std::wstring wide = Utf8ToWide(utf8Path);
    if (wide.empty()) return {};
    DWORD needed = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (needed == 0) return wide;
    std::wstring expanded(static_cast<size_t>(needed), L'\0');
    DWORD written = ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), needed);
    if (written == 0) return wide;
    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded;
}

static std::wstring GetKnownFolderHelper(REFKNOWNFOLDERID rfid) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &path)) && path) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return {};
}

std::wstring ActionDispatcher::ResolveKnownWindowsActionPath(std::string_view windowsActionName, bool* outIsUri) {
    if (outIsUri) *outIsUri = false;

    if (windowsActionName == "Documents") {
        return GetKnownFolderHelper(FOLDERID_Documents);
    }
    if (windowsActionName == "Downloads") {
        return GetKnownFolderHelper(FOLDERID_Downloads);
    }
    if (windowsActionName == "FileExplorer") {
        std::wstring winDir = GetKnownFolderHelper(FOLDERID_Windows);
        if (!winDir.empty()) {
            return (std::filesystem::path(winDir) / L"explorer.exe").wstring();
        }
        return L"C:\\Windows\\explorer.exe";
    }
    if (windowsActionName == "Notepad") {
        std::wstring sysDir = GetKnownFolderHelper(FOLDERID_System);
        if (!sysDir.empty()) {
            std::filesystem::path p = std::filesystem::path(sysDir) / L"notepad.exe";
            if (std::filesystem::exists(p)) return p.wstring();
        }
        std::wstring winDir = GetKnownFolderHelper(FOLDERID_Windows);
        if (!winDir.empty()) {
            return (std::filesystem::path(winDir) / L"notepad.exe").wstring();
        }
        return L"C:\\Windows\\System32\\notepad.exe";
    }
    if (windowsActionName == "TaskManager") {
        std::wstring sysDir = GetKnownFolderHelper(FOLDERID_System);
        if (!sysDir.empty()) {
            return (std::filesystem::path(sysDir) / L"Taskmgr.exe").wstring();
        }
        return L"C:\\Windows\\System32\\Taskmgr.exe";
    }
    if (windowsActionName == "WindowsSettings") {
        if (outIsUri) *outIsUri = true;
        return L"ms-settings:";
    }
    if (windowsActionName == "LockWorkstation") {
        return L"::LockWorkStation";
    }
    return {};
}

static bool IsBlockedShellInterpreter(const std::filesystem::path& exePath) {
    std::wstring filename = exePath.filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(), ::towlower);
    return filename == L"cmd.exe" ||
           filename == L"powershell.exe" ||
           filename == L"pwsh.exe" ||
           filename == L"wscript.exe" ||
           filename == L"cscript.exe" ||
           filename == L"mshta.exe";
}

ParsedShortcutChord ActionDispatcher::ParseShortcutChord(std::string_view chordText) {
    ParsedShortcutChord res;
    if (chordText.empty()) {
        res.error = "Shortcut chord is empty.";
        return res;
    }

    auto trim = [](std::string_view s) -> std::string {
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        std::string out(s.substr(a, b - a));
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return out;
    };

    size_t start = 0;
    while (start <= chordText.size()) {
        size_t plusPos = chordText.find('+', start);
        std::string_view tokenView = (plusPos == std::string_view::npos)
            ? chordText.substr(start)
            : chordText.substr(start, plusPos - start);
        std::string tok = trim(tokenView);
        if (tok.empty()) {
            res.error = "Empty token in shortcut chord.";
            return res;
        }

        if (tok == "CTRL" || tok == "CONTROL") {
            res.modifierVks.push_back(VK_CONTROL);
        } else if (tok == "ALT") {
            res.modifierVks.push_back(VK_MENU);
        } else if (tok == "SHIFT") {
            res.modifierVks.push_back(VK_SHIFT);
        } else if (tok == "WIN" || tok == "LWIN") {
            res.modifierVks.push_back(VK_LWIN);
        } else {
            if (res.primaryVk != 0) {
                res.error = "Multiple non-modifier keys in chord.";
                return res;
            }
            if (tok.size() == 1 && ((tok[0] >= 'A' && tok[0] <= 'Z') || (tok[0] >= '0' && tok[0] <= '9'))) {
                res.primaryVk = static_cast<uint16_t>(tok[0]);
            } else if (tok == "ESC" || tok == "ESCAPE") {
                res.primaryVk = VK_ESCAPE;
            } else if (tok == "SPACE") {
                res.primaryVk = VK_SPACE;
            } else if (tok == "TAB") {
                res.primaryVk = VK_TAB;
            } else if (tok == "ENTER" || tok == "RETURN") {
                res.primaryVk = VK_RETURN;
            } else if (tok == "BACKSPACE" || tok == "BACK") {
                res.primaryVk = VK_BACK;
            } else if (tok == "DELETE" || tok == "DEL") {
                res.primaryVk = VK_DELETE;
            } else if (tok == "INSERT" || tok == "INS") {
                res.primaryVk = VK_INSERT;
            } else if (tok == "HOME") {
                res.primaryVk = VK_HOME;
            } else if (tok == "END") {
                res.primaryVk = VK_END;
            } else if (tok == "PAGEUP" || tok == "PGUP") {
                res.primaryVk = VK_PRIOR;
            } else if (tok == "PAGEDOWN" || tok == "PGDN") {
                res.primaryVk = VK_NEXT;
            } else if (tok == "LEFT") {
                res.primaryVk = VK_LEFT;
            } else if (tok == "RIGHT") {
                res.primaryVk = VK_RIGHT;
            } else if (tok == "UP") {
                res.primaryVk = VK_UP;
            } else if (tok == "DOWN") {
                res.primaryVk = VK_DOWN;
            } else if (tok.size() >= 2 && tok[0] == 'F') {
                int fNum = std::atoi(tok.c_str() + 1);
                if (fNum >= 1 && fNum <= 24) {
                    res.primaryVk = static_cast<uint16_t>(VK_F1 + (fNum - 1));
                } else {
                    res.error = "Invalid function key: " + tok;
                    return res;
                }
            } else {
                res.error = "Unrecognized key token: " + tok;
                return res;
            }
        }

        if (plusPos == std::string_view::npos) break;
        start = plusPos + 1;
    }

    if (res.primaryVk == 0) {
        res.error = "Shortcut chord requires a primary key.";
        return res;
    }
    res.valid = true;
    return res;
}

bool ActionDispatcher::IsActionTargetAvailable(const ActionDefinition& action, std::string* outReason) {
    if (!action.enabled) {
        if (outReason) *outReason = "Action is disabled.";
        return false;
    }
    if (!action.approved) {
        if (outReason) *outReason = "Imported action awaits user review and approval.";
        return false;
    }

    switch (action.type) {
        case ActionType::None:
            if (outReason) *outReason = "No action assigned.";
            return false;

        case ActionType::LaunchApp: {
            std::wstring expanded = ExpandPathStrings(action.target);
            if (expanded.empty()) {
                if (outReason) *outReason = "Executable target path is empty.";
                return false;
            }
            std::filesystem::path p(expanded);
            if (IsBlockedShellInterpreter(p)) {
                if (outReason) *outReason = "Arbitrary shell interpreters (cmd/powershell) are disallowed.";
                return false;
            }
            DWORD attrs = GetFileAttributesW(expanded.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (outReason) *outReason = "Target executable or shortcut was not found.";
                return false;
            }
            return true;
        }

        case ActionType::OpenFile: {
            std::wstring expanded = ExpandPathStrings(action.target);
            if (expanded.empty()) {
                if (outReason) *outReason = "File target path is empty.";
                return false;
            }
            DWORD attrs = GetFileAttributesW(expanded.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (outReason) *outReason = "Target file does not exist.";
                return false;
            }
            return true;
        }

        case ActionType::OpenFolder: {
            std::wstring expanded = ExpandPathStrings(action.target);
            if (expanded.empty()) {
                if (outReason) *outReason = "Folder path is empty.";
                return false;
            }
            DWORD attrs = GetFileAttributesW(expanded.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                if (outReason) *outReason = "Target directory does not exist.";
                return false;
            }
            return true;
        }

        case ActionType::OpenUrl: {
            if (!action.target.starts_with("http://") && !action.target.starts_with("https://")) {
                if (outReason) *outReason = "URL must start with http:// or https://.";
                return false;
            }
            if (action.target.size() <= 8) {
                if (outReason) *outReason = "URL host is missing.";
                return false;
            }
            return true;
        }

        case ActionType::SendShortcut: {
            ParsedShortcutChord parsed = ParseShortcutChord(action.shortcutChord);
            if (!parsed.valid) {
                if (outReason) *outReason = parsed.error;
                return false;
            }
            return true;
        }

        case ActionType::WindowsAction: {
            bool isUri = false;
            std::wstring resolved = ResolveKnownWindowsActionPath(action.windowsActionName, &isUri);
            if (resolved.empty()) {
                if (outReason) *outReason = "Unrecognized WindowsAction facility.";
                return false;
            }
            if (!isUri && resolved != L"::LockWorkStation") {
                DWORD attrs = GetFileAttributesW(resolved.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES) {
                    if (outReason) *outReason = "Resolved system target path does not exist.";
                    return false;
                }
            }
            return true;
        }

        case ActionType::OpenKeychainSettings:
        case ActionType::OpenChildRing:
            return true;
    }

    return false;
}

ActionExecutionResult ActionDispatcher::ExecuteSynchronously(const CommittedAction& committed) {
    ActionExecutionResult res;
    res.generationId = committed.generationId;
    res.actionId     = committed.action.id;
    res.actionLabel  = committed.action.label;
    res.actionType   = committed.action.type;
    res.dispatchQpc  = clock_->NowTicks();

    // Previews never execute actions
    if (previewMode_.load(std::memory_order_acquire)) {
        res.status            = ActionExecutionStatus::PreviewSuppressed;
        res.diagnosticMessage = "Action execution suppressed while Preview mode is active.";
        return res;
    }

    const ActionDefinition& act = committed.action;
    if (!act.enabled) {
        res.status            = ActionExecutionStatus::DisabledActionBlocked;
        res.diagnosticMessage = "Action '" + act.label + "' is disabled.";
        return res;
    }
    if (!act.approved) {
        res.status            = ActionExecutionStatus::UnapprovedActionBlocked;
        res.diagnosticMessage = "Action '" + act.label + "' is unapproved and requires user review in Settings.";
        return res;
    }

    std::string availErr;
    if (!IsActionTargetAvailable(act, &availErr)) {
        if (act.type == ActionType::OpenUrl) {
            res.status = ActionExecutionStatus::InvalidUrlScheme;
        } else if (availErr.find("cmd/powershell") != std::string::npos) {
            res.status = ActionExecutionStatus::ShellInterpreterBlocked;
        } else {
            res.status = ActionExecutionStatus::TargetNotFound;
        }
        res.diagnosticMessage = availErr;
        return res;
    }

    if (dryRunForTests_.load(std::memory_order_acquire)) {
        // In unit test dry-run mode, still validate SendShortcut foreground identity
        if (act.type == ActionType::SendShortcut && committed.originalForeground != nullptr) {
            HWND currentFg = GetForegroundWindow();
            if (currentFg != nullptr && currentFg != committed.originalForeground) {
                res.status            = ActionExecutionStatus::ForegroundWindowChanged;
                res.diagnosticMessage = "Foreground window changed before SendShortcut injection.";
                return res;
            }
        }
        res.status            = ActionExecutionStatus::Succeeded;
        res.diagnosticMessage = "Dry-run action validated and dispatched.";
        return res;
    }

    switch (act.type) {
        case ActionType::OpenKeychainSettings: {
            LaunchSettingsCallback launchCb;
            AcquireSRWLockShared(&callbackLock_);
            launchCb = launchSettingsCallback_;
            ReleaseSRWLockShared(&callbackLock_);

            std::string err;
            if (launchCb && launchCb(err)) {
                res.status            = ActionExecutionStatus::Succeeded;
                res.diagnosticMessage = "Launched Keychain.Settings.exe.";
            } else {
                res.status            = ActionExecutionStatus::ShellExecuteFailed;
                res.diagnosticMessage = err.empty() ? "Failed to launch Keychain.Settings.exe." : err;
            }
            return res;
        }

        case ActionType::SendShortcut: {
            // Revalidate foreground window identity before injection
            if (committed.originalForeground != nullptr) {
                HWND currentFg = GetForegroundWindow();
                if (currentFg != committed.originalForeground) {
                    res.status            = ActionExecutionStatus::ForegroundWindowChanged;
                    res.diagnosticMessage = "Cancelled SendShortcut because foreground window changed.";
                    return res;
                }
            }

            ParsedShortcutChord chord = ParseShortcutChord(act.shortcutChord);
            if (!chord.valid) {
                res.status            = ActionExecutionStatus::TargetNotFound;
                res.diagnosticMessage = chord.error;
                return res;
            }

            std::vector<INPUT> inputs;
            inputs.reserve(chord.modifierVks.size() * 2 + 2);

            for (uint16_t modVk : chord.modifierVks) {
                INPUT in{};
                in.type           = INPUT_KEYBOARD;
                in.ki.wVk         = modVk;
                in.ki.dwFlags     = 0;
                in.ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;
                inputs.push_back(in);
            }
            {
                INPUT down{};
                down.type           = INPUT_KEYBOARD;
                down.ki.wVk         = chord.primaryVk;
                down.ki.dwFlags     = 0;
                down.ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;
                inputs.push_back(down);

                INPUT up{};
                up.type           = INPUT_KEYBOARD;
                up.ki.wVk         = chord.primaryVk;
                up.ki.dwFlags     = KEYEVENTF_KEYUP;
                up.ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;
                inputs.push_back(up);
            }
            for (auto it = chord.modifierVks.rbegin(); it != chord.modifierVks.rend(); ++it) {
                INPUT in{};
                in.type           = INPUT_KEYBOARD;
                in.ki.wVk         = *it;
                in.ki.dwFlags     = KEYEVENTF_KEYUP;
                in.ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;
                inputs.push_back(in);
            }

            SetLastError(0);
            const UINT expected = static_cast<UINT>(inputs.size());
            const UINT sent     = SendInput(expected, inputs.data(), sizeof(INPUT));
            if (sent != expected) {
                res.win32Error = GetLastError();
                // Handle partial SendInput completion: ensure any pressed modifiers are released
                std::vector<INPUT> cleanup;
                for (uint16_t modVk : chord.modifierVks) {
                    INPUT up{};
                    up.type           = INPUT_KEYBOARD;
                    up.ki.wVk         = modVk;
                    up.ki.dwFlags     = KEYEVENTF_KEYUP;
                    up.ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;
                    cleanup.push_back(up);
                }
                if (!cleanup.empty()) {
                    SendInput(static_cast<UINT>(cleanup.size()), cleanup.data(), sizeof(INPUT));
                }
                res.status = ActionExecutionStatus::UipiOrSendInputBlocked;
                res.diagnosticMessage = "SendInput blocked (likely UIPI elevated target window) or partially completed.";
                return res;
            }

            res.status            = ActionExecutionStatus::Succeeded;
            res.diagnosticMessage = "Shortcut chord injected.";
            return res;
        }

        case ActionType::WindowsAction: {
            if (act.windowsActionName == "LockWorkstation") {
                if (LockWorkStation()) {
                    res.status            = ActionExecutionStatus::Succeeded;
                    res.diagnosticMessage = "Workstation locked.";
                } else {
                    res.win32Error        = GetLastError();
                    res.status            = ActionExecutionStatus::ShellExecuteFailed;
                    res.diagnosticMessage = "LockWorkStation failed.";
                }
                return res;
            }

            bool isUri = false;
            std::wstring targetPath = ResolveKnownWindowsActionPath(act.windowsActionName, &isUri);
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask  = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"open";
            sei.lpFile = targetPath.c_str();
            sei.nShow  = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&sei)) {
                res.win32Error        = GetLastError();
                res.status            = ActionExecutionStatus::ShellExecuteFailed;
                res.diagnosticMessage = "ShellExecuteExW failed for WindowsAction '" + act.windowsActionName + "'.";
                return res;
            }
            res.status            = ActionExecutionStatus::Succeeded;
            res.diagnosticMessage = "Executed WindowsAction '" + act.windowsActionName + "'.";
            return res;
        }

        case ActionType::LaunchApp:
        case ActionType::OpenFile:
        case ActionType::OpenFolder:
        case ActionType::OpenUrl: {
            std::wstring fileWide = (act.type == ActionType::OpenUrl)
                ? Utf8ToWide(act.target)
                : ExpandPathStrings(act.target);
            // Keep executable paths and argument data strictly separate
            std::wstring argsWide = (act.type == ActionType::LaunchApp)
                ? Utf8ToWide(act.arguments)
                : std::wstring{};
            std::wstring workDirWide = ExpandPathStrings(act.workingDirectory);

            SHELLEXECUTEINFOW sei{};
            sei.cbSize       = sizeof(sei);
            sei.fMask        = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb       = L"open";
            sei.lpFile       = fileWide.c_str();
            sei.lpParameters = argsWide.empty() ? nullptr : argsWide.c_str();
            sei.lpDirectory  = workDirWide.empty() ? nullptr : workDirWide.c_str();
            sei.nShow        = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&sei)) {
                res.win32Error        = GetLastError();
                res.status            = ActionExecutionStatus::ShellExecuteFailed;
                res.diagnosticMessage = "ShellExecuteExW failed with Win32 error " + std::to_string(res.win32Error) + ".";
                return res;
            }
            res.status            = ActionExecutionStatus::Succeeded;
            res.diagnosticMessage = "Dispatched action '" + act.label + "'.";
            return res;
        }

        default:
            res.status            = ActionExecutionStatus::TargetNotFound;
            res.diagnosticMessage = "Unsupported action type for execution.";
            return res;
    }
}

} // namespace keychain
