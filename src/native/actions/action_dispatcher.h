#pragma once

#include "common/types.h"
#include "config/config_schema.h"
#include "gesture/gesture_state_machine.h"
#include <functional>
#include <thread>
#include <deque>
#include <atomic>
#include <vector>
#include <filesystem>

namespace keychain {

enum class ActionExecutionStatus {
    Succeeded,
    PreviewSuppressed,
    UnapprovedActionBlocked,
    DisabledActionBlocked,
    TargetNotFound,
    InvalidUrlScheme,
    ShellInterpreterBlocked,
    ForegroundWindowChanged,
    UipiOrSendInputBlocked,
    ShellExecuteFailed
};

const char* ActionExecutionStatusToString(ActionExecutionStatus status) noexcept;

struct ActionExecutionResult {
    uint64_t              generationId  = 0;
    std::string           actionId;
    std::string           actionLabel;
    ActionType            actionType    = ActionType::None;
    ActionExecutionStatus status        = ActionExecutionStatus::Succeeded;
    DWORD                 win32Error    = 0;
    int64_t               dispatchQpc   = 0;
    std::string           diagnosticMessage;
};

struct ParsedShortcutChord {
    bool                  valid = false;
    std::vector<uint16_t> modifierVks;
    uint16_t              primaryVk = 0;
    std::string           error;
};

class ActionDispatcher {
public:
    using CompletionCallback = std::function<void(const ActionExecutionResult&)>;
    using LaunchSettingsCallback = std::function<bool(std::string& outError)>;

    explicit ActionDispatcher(std::shared_ptr<IClock> clock = std::make_shared<SystemQpcClock>());
    ~ActionDispatcher();

    ActionDispatcher(const ActionDispatcher&) = delete;
    ActionDispatcher& operator=(const ActionDispatcher&) = delete;

    void Start();
    void Stop();

    void SetPreviewMode(bool previewActive) noexcept {
        previewMode_.store(previewActive, std::memory_order_release);
    }
    bool IsPreviewMode() const noexcept {
        return previewMode_.load(std::memory_order_acquire);
    }

    void SetDryRunForTests(bool dryRun) noexcept {
        dryRunForTests_.store(dryRun, std::memory_order_release);
    }

    void SetCompletionCallback(CompletionCallback cb);
    void SetLaunchSettingsCallback(LaunchSettingsCallback cb);

    // Enqueues committed action for asynchronous execution on the COM STA worker thread
    void EnqueueCommittedAction(CommittedAction committed);

    // Synchronous execution helper (used by worker thread and deterministic tests)
    ActionExecutionResult ExecuteSynchronously(const CommittedAction& committed);

    // Static target resolution & validation helpers
    static bool IsActionTargetAvailable(const ActionDefinition& action, std::string* outReason = nullptr);
    static ParsedShortcutChord ParseShortcutChord(std::string_view chordText);
    static std::wstring ResolveKnownWindowsActionPath(std::string_view windowsActionName, bool* outIsUri = nullptr);
    static std::wstring ExpandPathStrings(std::string_view utf8Path);

private:
    void WorkerThreadMain();

    std::shared_ptr<IClock> clock_;
    std::atomic<bool> running_{false};
    std::atomic<bool> previewMode_{false};
    std::atomic<bool> dryRunForTests_{false};

    UniqueHandle workWakeEvent_;
    std::thread workerThread_;

    mutable SRWLOCK queueLock_ = SRWLOCK_INIT;
    std::deque<CommittedAction> workQueue_;

    mutable SRWLOCK callbackLock_ = SRWLOCK_INIT;
    CompletionCallback completionCallback_;
    LaunchSettingsCallback launchSettingsCallback_;
};

} // namespace keychain
