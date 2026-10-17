#pragma once

#include "common/types.h"
#include "config/config_schema.h"
#include "layout/radial_layout.h"
#include <memory>
#include <optional>
#include <deque>
#include <functional>

namespace keychain {

enum class GestureState {
    Idle,
    Opening,
    Active,
    Closing,
    Suspended
};

enum class PendingActionState {
    None,
    WaitingForModifierRelease,
    ReadyToInject,
    Dispatched,
    Cancelled
};

enum class CancelReason {
    None,
    CenterRelease,
    EscapeKey,
    WinReleasedFirst,
    RightClickRoot,
    SessionLockedOrDesktopLost,
    InvalidOrDisabledTarget,
    ChildRingLinkNotEntered,
    QueueOverflow,
    SuspendedOrShuttingDown,
    ForegroundChangedBeforeShortcut,
    ShortcutModifierReleaseTimeout
};

const char* GestureStateToString(GestureState s) noexcept;
const char* CancelReasonToString(CancelReason r) noexcept;

struct CommittedAction {
    uint64_t                               generationId       = 0;
    ActionDefinition                       action             = {};
    HWND                                   originalForeground = nullptr;
    DWORD                                  originalProcessId  = 0;
    int64_t                                receiptQpc         = 0;
    int64_t                                recognizedQpc      = 0;
    int64_t                                commitQpc          = 0;
    std::shared_ptr<const ConfigDocument>  configSnapshot;
};

struct GestureStepResult {
    bool                           consumeInput         = false;
    bool                           emitStartMenuMask    = false;
    bool                           stateChanged         = false;
    bool                           selectionChanged     = false;
    bool                           navigatedChildRing   = false;
    bool                           returnedToRootRing   = false;
    CancelReason                   cancelReason         = CancelReason::None;
    std::optional<CommittedAction> immediateCommit;     // Non-shortcut or modifier-free commit ready now
    std::optional<CommittedAction> readyDeferredCommit; // SendShortcut ready after physical modifiers released
};

class GestureStateMachine {
public:
    using TargetAvailabilityCheck = std::function<bool(const ActionDefinition&)>;

    explicit GestureStateMachine(
        std::shared_ptr<IClock> clock = std::make_shared<SystemQpcClock>(),
        TargetAvailabilityCheck targetCheck = nullptr
    );

    void SetConfigSnapshotProvider(std::function<std::shared_ptr<const ConfigDocument>()> provider);
    void SetTargetAvailabilityCheck(TargetAvailabilityCheck check);

    GestureState GetState() const noexcept { return state_; }
    PendingActionState GetPendingActionState() const noexcept { return pendingState_; }
    uint64_t GetCurrentGenerationId() const noexcept { return generationId_; }
    CancelReason GetLastCancelReason() const noexcept { return lastCancelReason_; }

    bool IsCapsOwnedByGesture() const noexcept { return capsOwnedByGesture_; }
    bool GetPreexistingCapsToggleState() const noexcept { return preexistingCapsToggleOn_; }
    bool IsInChildRing() const noexcept { return inChildRing_; }
    int  GetSelectedSector() const noexcept { return selectedSector_; }
    int  GetDwellingSector() const noexcept { return dwellingSector_; }
    float GetDwellProgress(int64_t nowTicks) const noexcept;

    const RadialLayout& GetCurrentLayout() const noexcept { return currentLayout_; }
    std::shared_ptr<const ConfigDocument> GetActiveConfigSnapshot() const noexcept { return activeConfig_; }
    const RadialRing* GetActiveRing() const noexcept;

    // Input event handlers
    GestureStepResult OnKeyEvent(
        uint32_t vkCode,
        bool isDown,
        bool isRepeat,
        bool capsToggleStateBeforeEvent,
        Point2I cursorScreenPx,
        RectI monitorWorkAreaPx,
        uint32_t dpi,
        HWND currentForegroundHwnd
    );

    GestureStepResult OnHotKeyChordTriggered(
        Point2I cursorScreenPx,
        RectI monitorWorkAreaPx,
        uint32_t dpi,
        HWND currentForegroundHwnd
    );

    GestureStepResult OnPointerMove(Point2I cursorScreenPx);

    GestureStepResult OnMouseButton(
        bool isRightButton,
        bool isDown,
        Point2I cursorScreenPx
    );

    // Time progression for opening->active transition, closing->idle completion,
    // child-ring 250ms hover dwell, and deferred SendShortcut modifier-release timeout.
    GestureStepResult AdvanceTime(
        int64_t nowTicks,
        HWND currentForegroundHwnd
    );

    // System/lifecycle transitions
    GestureStepResult OnSessionOrDesktopLost();
    GestureStepResult OnQueueOverflow();
    void SetSuspended(bool suspended);
    void ResetAllState();

private:
    bool AreAnyActivationModifiersDown() const noexcept;
    void RebuildLayoutForActiveRing(Point2I anchorPx, RectI workAreaPx, uint32_t dpi);
    void UpdateSelectionFromCursor(Point2I cursorPx, int64_t nowTicks, GestureStepResult& outResult);
    void CancelActiveGesture(CancelReason reason, int64_t nowTicks, GestureStepResult& outResult);
    void CommitCurrentSelection(int64_t nowTicks, HWND currentForegroundHwnd, GestureStepResult& outResult);

    std::shared_ptr<IClock> clock_;
    std::function<std::shared_ptr<const ConfigDocument>()> configProvider_;
    TargetAvailabilityCheck targetCheck_;

    GestureState state_ = GestureState::Idle;
    uint64_t generationId_ = 0;
    uint64_t lastCommittedGenerationId_ = 0;
    CancelReason lastCancelReason_ = CancelReason::None;

    // Tracked physical key states from hook events (never GetAsyncKeyState)
    bool leftWinDown_             = false;
    bool rightWinDown_            = false;
    bool capsDown_                = false;
    bool ctrlDown_                = false;
    bool altDown_                 = false;
    bool shiftDown_               = false;
    bool capsEnteredBeforeWin_    = false;
    bool capsOwnedByGesture_      = false;
    bool preexistingCapsToggleOn_ = false;
    bool consumeMatchingRightUp_  = false;
    bool consumeMatchingLeftUp_   = false;

    // Active gesture snapshot & ring navigation
    std::shared_ptr<const ConfigDocument> activeConfig_;
    std::string rootRingId_;
    std::string activeRingId_;
    bool inChildRing_ = false;
    bool isToggleSession_ = false;

    RadialLayout currentLayout_{};
    Point2I latestCursorPx_{};
    int selectedSector_ = -1;

    // Dwell tracking for child-ring sectors
    int dwellingSector_ = -1;
    int64_t dwellStartTicks_ = 0;

    // Lifecycle timestamps
    int64_t gestureReceiptTicks_ = 0;
    int64_t gestureRecognizedTicks_ = 0;
    int64_t stateEnterTicks_ = 0;

    HWND capturedForegroundHwnd_ = nullptr;
    DWORD capturedForegroundPid_ = 0;

    // Deferred SendShortcut state
    PendingActionState pendingState_ = PendingActionState::None;
    std::optional<CommittedAction> pendingShortcut_;
    int64_t pendingShortcutQueuedTicks_ = 0;
};

} // namespace keychain
