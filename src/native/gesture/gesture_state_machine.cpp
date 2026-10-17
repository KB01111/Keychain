#include "gesture/gesture_state_machine.h"

namespace keychain {

const char* GestureStateToString(GestureState s) noexcept {
    switch (s) {
        case GestureState::Idle:      return "Idle";
        case GestureState::Opening:   return "Opening";
        case GestureState::Active:    return "Active";
        case GestureState::Closing:   return "Closing";
        case GestureState::Suspended: return "Suspended";
    }
    return "Unknown";
}

const char* CancelReasonToString(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::None:                            return "None";
        case CancelReason::CenterRelease:                   return "CenterRelease";
        case CancelReason::EscapeKey:                       return "EscapeKey";
        case CancelReason::WinReleasedFirst:                return "WinReleasedFirst";
        case CancelReason::RightClickRoot:                  return "RightClickRoot";
        case CancelReason::SessionLockedOrDesktopLost:      return "SessionLockedOrDesktopLost";
        case CancelReason::InvalidOrDisabledTarget:         return "InvalidOrDisabledTarget";
        case CancelReason::ChildRingLinkNotEntered:         return "ChildRingLinkNotEntered";
        case CancelReason::QueueOverflow:                   return "QueueOverflow";
        case CancelReason::SuspendedOrShuttingDown:         return "SuspendedOrShuttingDown";
        case CancelReason::ForegroundChangedBeforeShortcut: return "ForegroundChangedBeforeShortcut";
        case CancelReason::ShortcutModifierReleaseTimeout:  return "ShortcutModifierReleaseTimeout";
    }
    return "Unknown";
}

GestureStateMachine::GestureStateMachine(
    std::shared_ptr<IClock> clock,
    TargetAvailabilityCheck targetCheck
)
    : clock_(std::move(clock))
    , targetCheck_(std::move(targetCheck)) {
    if (!clock_) {
        clock_ = std::make_shared<SystemQpcClock>();
    }
    activeConfig_ = std::make_shared<const ConfigDocument>(ConfigStore::CreateDefaultConfig());
}

void GestureStateMachine::SetConfigSnapshotProvider(std::function<std::shared_ptr<const ConfigDocument>()> provider) {
    configProvider_ = std::move(provider);
}

void GestureStateMachine::SetTargetAvailabilityCheck(TargetAvailabilityCheck check) {
    targetCheck_ = std::move(check);
}

const RadialRing* GestureStateMachine::GetActiveRing() const noexcept {
    if (!activeConfig_) return nullptr;
    return activeConfig_->FindRing(activeRingId_);
}

bool GestureStateMachine::AreAnyActivationModifiersDown() const noexcept {
    return leftWinDown_ || rightWinDown_ || capsDown_ || ctrlDown_ || altDown_;
}

float GestureStateMachine::GetDwellProgress(int64_t nowTicks) const noexcept {
    if (dwellingSector_ < 0 || inChildRing_ || (state_ != GestureState::Opening && state_ != GestureState::Active)) {
        return 0.0f;
    }
    const uint32_t dwellMs = activeConfig_ ? activeConfig_->preferences.childRingDwellMs : 250u;
    const double elapsedMs = clock_->TicksToMilliseconds(nowTicks - dwellStartTicks_);
    return static_cast<float>(std::clamp(elapsedMs / static_cast<double>(std::max(dwellMs, 1u)), 0.0, 1.0));
}

void GestureStateMachine::RebuildLayoutForActiveRing(Point2I anchorPx, RectI workAreaPx, uint32_t dpi) {
    uint32_t sectorCount = 6;
    float diameterDip = 380.0f;
    float deadZoneDip = 44.0f;
    float gapDip = 3.5f;

    if (activeConfig_) {
        diameterDip = activeConfig_->preferences.ringDiameterDip;
        deadZoneDip = activeConfig_->preferences.deadZoneRadiusDip;
        gapDip      = activeConfig_->preferences.sectorGapDip;
        sectorCount = activeConfig_->preferences.defaultSectorCount;
        if (const RadialRing* ring = activeConfig_->FindRing(activeRingId_)) {
            sectorCount = ring->sectorCount;
        }
    }

    currentLayout_ = RadialLayout::Compute(
        anchorPx,
        workAreaPx,
        dpi,
        sectorCount,
        diameterDip,
        deadZoneDip,
        gapDip
    );
}

void GestureStateMachine::UpdateSelectionFromCursor(Point2I cursorPx, int64_t nowTicks, GestureStepResult& outResult) {
    latestCursorPx_ = cursorPx;
    const HitTestResult hit = currentLayout_.HitTest(cursorPx, selectedSector_);
    if (hit.selectedSector != selectedSector_) {
        selectedSector_ = hit.selectedSector;
        outResult.selectionChanged = true;

        // Check if newly selected sector is a child-ring link at root level
        dwellingSector_ = -1;
        if (!inChildRing_ && selectedSector_ >= 0 && activeConfig_) {
            if (const RadialRing* ring = GetActiveRing()) {
                if (static_cast<size_t>(selectedSector_) < ring->sectorActionIds.size()) {
                    const auto* act = activeConfig_->FindAction(ring->sectorActionIds[static_cast<size_t>(selectedSector_)]);
                    if (act && act->enabled && act->type == ActionType::OpenChildRing) {
                        dwellingSector_ = selectedSector_;
                        dwellStartTicks_ = nowTicks;
                    }
                }
            }
        }
    }
}

void GestureStateMachine::CancelActiveGesture(CancelReason reason, int64_t nowTicks, GestureStepResult& outResult) {
    lastCancelReason_ = reason;
    outResult.cancelReason = reason;
    selectedSector_ = -1;
    dwellingSector_ = -1;
    inChildRing_ = false;
    isToggleSession_ = false;

    if (state_ == GestureState::Opening || state_ == GestureState::Active) {
        state_ = GestureState::Closing;
        stateEnterTicks_ = nowTicks;
        outResult.stateChanged = true;
    }
}

void GestureStateMachine::CommitCurrentSelection(int64_t nowTicks, HWND currentForegroundHwnd, GestureStepResult& outResult) {
    // Prevent duplicate dispatch for the same generation ID
    if (generationId_ == 0 || generationId_ == lastCommittedGenerationId_) {
        CancelActiveGesture(CancelReason::CenterRelease, nowTicks, outResult);
        return;
    }

    // Re-evaluate hit test at latestCursorPx_ so even if zero render frames occurred,
    // selection is accurate to the latest cursor position.
    if (!isToggleSession_) {
        const HitTestResult hit = currentLayout_.HitTest(latestCursorPx_, selectedSector_);
        selectedSector_ = hit.selectedSector;
        if (hit.inDeadZone || selectedSector_ < 0) {
            CancelActiveGesture(CancelReason::CenterRelease, nowTicks, outResult);
            return;
        }
    } else if (selectedSector_ < 0) {
        CancelActiveGesture(CancelReason::CenterRelease, nowTicks, outResult);
        return;
    }

    const RadialRing* ring = GetActiveRing();
    if (!ring || !activeConfig_ || static_cast<size_t>(selectedSector_) >= ring->sectorActionIds.size()) {
        CancelActiveGesture(CancelReason::InvalidOrDisabledTarget, nowTicks, outResult);
        return;
    }

    const std::string& actId = ring->sectorActionIds[static_cast<size_t>(selectedSector_)];
    if (actId.empty()) {
        CancelActiveGesture(CancelReason::InvalidOrDisabledTarget, nowTicks, outResult);
        return;
    }

    const ActionDefinition* act = activeConfig_->FindAction(actId);
    if (!act || act->type == ActionType::None) {
        CancelActiveGesture(CancelReason::InvalidOrDisabledTarget, nowTicks, outResult);
        return;
    }

    // Releasing over a child-ring link before navigation cancels instead of executing another action
    if (act->type == ActionType::OpenChildRing) {
        if (isToggleSession_ && !inChildRing_) {
            // In keyboard toggle mode, Enter on a child ring enters the child ring
            if (const RadialRing* childRing = activeConfig_->FindRing(act->childRingId)) {
                inChildRing_ = true;
                activeRingId_ = childRing->id;
                dwellingSector_ = -1;
                RebuildLayoutForActiveRing(currentLayout_.anchorScreenPx, currentLayout_.monitorWorkAreaPx, currentLayout_.dpi);
                selectedSector_ = 0;
                outResult.navigatedChildRing = true;
                outResult.selectionChanged = true;
                return;
            }
        }
        CancelActiveGesture(CancelReason::ChildRingLinkNotEntered, nowTicks, outResult);
        return;
    }

    if (!act->enabled || !act->approved) {
        CancelActiveGesture(CancelReason::InvalidOrDisabledTarget, nowTicks, outResult);
        return;
    }

    if (targetCheck_ && !targetCheck_(*act)) {
        CancelActiveGesture(CancelReason::InvalidOrDisabledTarget, nowTicks, outResult);
        return;
    }

    // Valid commit! Mark generation as committed so no duplicate can ever fire
    lastCommittedGenerationId_ = generationId_;
    lastCancelReason_ = CancelReason::None;

    CommittedAction committed;
    committed.generationId       = generationId_;
    committed.action             = *act;
    committed.originalForeground = capturedForegroundHwnd_;
    committed.originalProcessId  = capturedForegroundPid_;
    committed.receiptQpc         = gestureReceiptTicks_;
    committed.recognizedQpc      = gestureRecognizedTicks_;
    committed.commitQpc          = nowTicks;
    committed.configSnapshot     = activeConfig_;

    state_ = GestureState::Closing;
    stateEnterTicks_ = nowTicks;
    isToggleSession_ = false;
    dwellingSector_ = -1;
    outResult.stateChanged = true;

    // For SendShortcut: if any activation modifier is still physically held, queue and wait
    // event-driven for modifiers to be released before injecting.
    if (act->type == ActionType::SendShortcut) {
        if (AreAnyActivationModifiersDown()) {
            pendingState_ = PendingActionState::WaitingForModifierRelease;
            pendingShortcut_ = std::move(committed);
            pendingShortcutQueuedTicks_ = nowTicks;
        } else {
            // Revalidate foreground identity before immediate injection
            if (capturedForegroundHwnd_ != nullptr && currentForegroundHwnd != nullptr &&
                currentForegroundHwnd != capturedForegroundHwnd_) {
                pendingState_ = PendingActionState::Cancelled;
                lastCancelReason_ = CancelReason::ForegroundChangedBeforeShortcut;
                outResult.cancelReason = CancelReason::ForegroundChangedBeforeShortcut;
            } else {
                pendingState_ = PendingActionState::ReadyToInject;
                outResult.readyDeferredCommit = std::move(committed);
                pendingState_ = PendingActionState::Dispatched;
            }
        }
    } else {
        outResult.immediateCommit = std::move(committed);
    }
}

GestureStepResult GestureStateMachine::OnKeyEvent(
    uint32_t vkCode,
    bool isDown,
    bool /*isRepeat*/,
    bool capsToggleStateBeforeEvent,
    Point2I cursorScreenPx,
    RectI monitorWorkAreaPx,
    uint32_t dpi,
    HWND currentForegroundHwnd
) {
    GestureStepResult result;
    const int64_t nowTicks = clock_->NowTicks();

    // Update tracked modifier state from events (never GetAsyncKeyState)
    if (vkCode == VK_LWIN) {
        leftWinDown_ = isDown;
    } else if (vkCode == VK_RWIN) {
        rightWinDown_ = isDown;
    } else if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) {
        ctrlDown_ = isDown;
    } else if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) {
        altDown_ = isDown;
    } else if (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) {
        shiftDown_ = isDown;
    }

    if (state_ == GestureState::Suspended) {
        if (vkCode == VK_CAPITAL && !isDown) {
            capsDown_ = false;
            capsEnteredBeforeWin_ = false;
            if (capsOwnedByGesture_) {
                capsOwnedByGesture_ = false;
                result.consumeInput = true;
            }
        }
        return result;
    }

    const bool anyWinDown = leftWinDown_ || rightWinDown_;

    // Handle Caps Lock transitions
    if (vkCode == VK_CAPITAL) {
        if (isDown) {
            // If gesture already owns Caps Lock, suppress key repeats
            if (capsOwnedByGesture_) {
                result.consumeInput = true;
                return result;
            }

            // If Caps Lock was pressed BEFORE Win, retain ordinary Caps Lock behavior
            if (!anyWinDown || capsEnteredBeforeWin_) {
                capsDown_ = true;
                capsEnteredBeforeWin_ = true;
                result.consumeInput = false;
                return result;
            }

            // Check if configuration uses WinCaps activation and is enabled
            auto nextSnapshot = configProvider_ ? configProvider_() : activeConfig_;
            if (!nextSnapshot || !nextSnapshot->preferences.enabled ||
                nextSnapshot->preferences.activationMode != "WinCaps") {
                capsDown_ = true;
                result.consumeInput = false;
                return result;
            }

            // Ignore if unrelated modifiers (Ctrl/Alt) are held during Win+Caps activation
            if (ctrlDown_ || altDown_) {
                capsDown_ = true;
                result.consumeInput = false;
                return result;
            }

            // Recognized Win-down followed by Caps-down!
            capsDown_                = true;
            capsOwnedByGesture_      = true;
            preexistingCapsToggleOn_ = capsToggleStateBeforeEvent;
            result.consumeInput      = true;
            result.emitStartMenuMask = true;

            // Cancel any stale pending shortcut from an earlier generation
            if (pendingState_ == PendingActionState::WaitingForModifierRelease) {
                pendingState_ = PendingActionState::Cancelled;
                pendingShortcut_.reset();
            }

            // Start new generation (supports immediate reactivation during Closing)
            ++generationId_;
            activeConfig_           = std::move(nextSnapshot);
            gestureReceiptTicks_    = nowTicks;
            gestureRecognizedTicks_ = nowTicks;
            stateEnterTicks_        = nowTicks;
            state_                  = GestureState::Opening;
            result.stateChanged     = true;
            lastCancelReason_       = CancelReason::None;

            capturedForegroundHwnd_ = currentForegroundHwnd;
            capturedForegroundPid_  = 0;
            if (currentForegroundHwnd) {
                GetWindowThreadProcessId(currentForegroundHwnd, &capturedForegroundPid_);
            }

            const Profile* prof = activeConfig_->FindActiveProfile();
            rootRingId_      = prof ? prof->rootRingId : "ring_root_default";
            activeRingId_    = rootRingId_;
            inChildRing_     = false;
            isToggleSession_ = (activeConfig_->preferences.interactionMode == "Toggle");
            selectedSector_  = -1;
            dwellingSector_  = -1;

            RebuildLayoutForActiveRing(cursorScreenPx, monitorWorkAreaPx, dpi);
            UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
            if (isToggleSession_ && selectedSector_ < 0) {
                selectedSector_ = 0;
                result.selectionChanged = true;
            }
            return result;
        } else {
            // Caps Lock key-up
            capsDown_ = false;
            capsEnteredBeforeWin_ = false;

            if (capsOwnedByGesture_) {
                capsOwnedByGesture_ = false;
                result.consumeInput = true;

                // In Hold mode, releasing Caps Lock while Opening or Active commits!
                if (!isToggleSession_ && (state_ == GestureState::Opening || state_ == GestureState::Active)) {
                    latestCursorPx_ = cursorScreenPx;
                    CommitCurrentSelection(nowTicks, currentForegroundHwnd, result);
                }
            }
        }
    }

    // Handle Win-key release while Hold gesture is active: releasing Win first cancels!
    if ((vkCode == VK_LWIN || vkCode == VK_RWIN) && !isDown) {
        if (!anyWinDown && !isToggleSession_ &&
            (state_ == GestureState::Opening || state_ == GestureState::Active)) {
            CancelActiveGesture(CancelReason::WinReleasedFirst, nowTicks, result);
        }
    }

    // Handle Escape key during Opening or Active: always cancels and consumes Escape
    if (vkCode == VK_ESCAPE && isDown &&
        (state_ == GestureState::Opening || state_ == GestureState::Active)) {
        result.consumeInput = true;
        CancelActiveGesture(CancelReason::EscapeKey, nowTicks, result);
        return result;
    }

    // Handle keyboard navigation when active (Toggle mode or directional keys)
    if (isDown && (state_ == GestureState::Opening || state_ == GestureState::Active)) {
        const uint32_t count = currentLayout_.sectorCount;
        if (vkCode == VK_RIGHT || vkCode == VK_DOWN || (vkCode == VK_TAB && !shiftDown_)) {
            result.consumeInput = true;
            selectedSector_ = (selectedSector_ < 0) ? 0 : static_cast<int>((selectedSector_ + 1) % count);
            result.selectionChanged = true;
            return result;
        }
        if (vkCode == VK_LEFT || vkCode == VK_UP || (vkCode == VK_TAB && shiftDown_)) {
            result.consumeInput = true;
            selectedSector_ = (selectedSector_ <= 0) ? static_cast<int>(count - 1) : (selectedSector_ - 1);
            result.selectionChanged = true;
            return result;
        }
        if (vkCode >= '1' && vkCode <= '8') {
            uint32_t idx = vkCode - '1';
            if (idx < count) {
                result.consumeInput = true;
                selectedSector_ = static_cast<int>(idx);
                result.selectionChanged = true;
                return result;
            }
        }
        if (vkCode == VK_BACK && inChildRing_) {
            result.consumeInput = true;
            inChildRing_ = false;
            activeRingId_ = rootRingId_;
            dwellingSector_ = -1;
            RebuildLayoutForActiveRing(currentLayout_.anchorScreenPx, currentLayout_.monitorWorkAreaPx, currentLayout_.dpi);
            result.returnedToRootRing = true;
            result.selectionChanged = true;
            return result;
        }
        if (vkCode == VK_RETURN || (isToggleSession_ && vkCode == VK_SPACE)) {
            result.consumeInput = true;
            CommitCurrentSelection(nowTicks, currentForegroundHwnd, result);
            return result;
        }
    }

    // Check if a pending SendShortcut was waiting for all physical activation modifiers to release
    if (!isDown && pendingState_ == PendingActionState::WaitingForModifierRelease && pendingShortcut_.has_value()) {
        if (!AreAnyActivationModifiersDown()) {
            const double waitedMs = clock_->TicksToMilliseconds(nowTicks - pendingShortcutQueuedTicks_);
            if (waitedMs > 1500.0) {
                pendingState_ = PendingActionState::Cancelled;
                lastCancelReason_ = CancelReason::ShortcutModifierReleaseTimeout;
                result.cancelReason = CancelReason::ShortcutModifierReleaseTimeout;
                pendingShortcut_.reset();
            } else if (pendingShortcut_->originalForeground != nullptr &&
                       currentForegroundHwnd != nullptr &&
                       currentForegroundHwnd != pendingShortcut_->originalForeground) {
                pendingState_ = PendingActionState::Cancelled;
                lastCancelReason_ = CancelReason::ForegroundChangedBeforeShortcut;
                result.cancelReason = CancelReason::ForegroundChangedBeforeShortcut;
                pendingShortcut_.reset();
            } else {
                pendingState_ = PendingActionState::ReadyToInject;
                result.readyDeferredCommit = std::move(*pendingShortcut_);
                pendingShortcut_.reset();
                pendingState_ = PendingActionState::Dispatched;
            }
        }
    }

    return result;
}

GestureStepResult GestureStateMachine::OnHotKeyChordTriggered(
    Point2I cursorScreenPx,
    RectI monitorWorkAreaPx,
    uint32_t dpi,
    HWND currentForegroundHwnd
) {
    GestureStepResult result;
    if (state_ == GestureState::Suspended) {
        return result;
    }

    const int64_t nowTicks = clock_->NowTicks();

    // If already open in Toggle mode, pressing Ctrl+Alt+Space again cancels
    if (state_ == GestureState::Opening || state_ == GestureState::Active) {
        CancelActiveGesture(CancelReason::EscapeKey, nowTicks, result);
        return result;
    }

    auto nextSnapshot = configProvider_ ? configProvider_() : activeConfig_;
    if (!nextSnapshot || !nextSnapshot->preferences.enabled) {
        return result;
    }

    ++generationId_;
    activeConfig_           = std::move(nextSnapshot);
    gestureReceiptTicks_    = nowTicks;
    gestureRecognizedTicks_ = nowTicks;
    stateEnterTicks_        = nowTicks;
    state_                  = GestureState::Opening;
    result.stateChanged     = true;
    lastCancelReason_       = CancelReason::None;

    capturedForegroundHwnd_ = currentForegroundHwnd;
    capturedForegroundPid_  = 0;
    if (currentForegroundHwnd) {
        GetWindowThreadProcessId(currentForegroundHwnd, &capturedForegroundPid_);
    }

    const Profile* prof = activeConfig_->FindActiveProfile();
    rootRingId_      = prof ? prof->rootRingId : "ring_root_default";
    activeRingId_    = rootRingId_;
    inChildRing_     = false;
    isToggleSession_ = true; // Ctrl+Alt+Space operates as toggle/click/Enter session
    selectedSector_  = 0;
    dwellingSector_  = -1;

    RebuildLayoutForActiveRing(cursorScreenPx, monitorWorkAreaPx, dpi);
    UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
    if (selectedSector_ < 0) {
        selectedSector_ = 0;
        result.selectionChanged = true;
    }
    return result;
}

GestureStepResult GestureStateMachine::OnPointerMove(Point2I cursorScreenPx) {
    GestureStepResult result;
    latestCursorPx_ = cursorScreenPx;
    if (state_ != GestureState::Opening && state_ != GestureState::Active) {
        return result;
    }
    const int64_t nowTicks = clock_->NowTicks();
    UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
    return result;
}

GestureStepResult GestureStateMachine::OnMouseButton(
    bool isRightButton,
    bool isDown,
    Point2I cursorScreenPx
) {
    GestureStepResult result;
    const int64_t nowTicks = clock_->NowTicks();
    latestCursorPx_ = cursorScreenPx;

    if (isRightButton) {
        if (!isDown) {
            if (consumeMatchingRightUp_) {
                consumeMatchingRightUp_ = false;
                result.consumeInput = true;
            }
            return result;
        }

        // Right-button down during active gesture
        if (state_ == GestureState::Opening || state_ == GestureState::Active) {
            result.consumeInput = true;
            consumeMatchingRightUp_ = true;

            if (inChildRing_) {
                // Return one level to root ring
                inChildRing_ = false;
                activeRingId_ = rootRingId_;
                dwellingSector_ = -1;
                RebuildLayoutForActiveRing(currentLayout_.anchorScreenPx, currentLayout_.monitorWorkAreaPx, currentLayout_.dpi);
                UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
                result.returnedToRootRing = true;
                result.selectionChanged = true;
            } else {
                // Cancel at root ring without opening context menu
                CancelActiveGesture(CancelReason::RightClickRoot, nowTicks, result);
            }
        }
        return result;
    } else {
        // Left mouse button
        if (!isDown) {
            if (consumeMatchingLeftUp_) {
                consumeMatchingLeftUp_ = false;
                result.consumeInput = true;
            }
            return result;
        }

        if (state_ == GestureState::Opening || state_ == GestureState::Active) {
            UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
            if (selectedSector_ >= 0 && activeConfig_) {
                if (const RadialRing* ring = GetActiveRing()) {
                    if (static_cast<size_t>(selectedSector_) < ring->sectorActionIds.size()) {
                        const auto* act = activeConfig_->FindAction(ring->sectorActionIds[static_cast<size_t>(selectedSector_)]);
                        if (act && act->enabled && act->type == ActionType::OpenChildRing && !inChildRing_) {
                            if (const RadialRing* childRing = activeConfig_->FindRing(act->childRingId)) {
                                result.consumeInput = true;
                                consumeMatchingLeftUp_ = true;
                                inChildRing_ = true;
                                activeRingId_ = childRing->id;
                                dwellingSector_ = -1;
                                RebuildLayoutForActiveRing(currentLayout_.anchorScreenPx, currentLayout_.monitorWorkAreaPx, currentLayout_.dpi);
                                UpdateSelectionFromCursor(cursorScreenPx, nowTicks, result);
                                result.navigatedChildRing = true;
                                result.selectionChanged = true;
                                return result;
                            }
                        }
                    }
                }
            }

            if (isToggleSession_) {
                result.consumeInput = true;
                consumeMatchingLeftUp_ = true;
                CommitCurrentSelection(nowTicks, capturedForegroundHwnd_, result);
            }
        }
        return result;
    }
}

GestureStepResult GestureStateMachine::AdvanceTime(
    int64_t nowTicks,
    HWND currentForegroundHwnd
) {
    GestureStepResult result;

    if (state_ == GestureState::Opening) {
        const double elapsedMs = clock_->TicksToMilliseconds(nowTicks - stateEnterTicks_);
        const double openDurationMs = 140.0 / (activeConfig_ ? std::max(activeConfig_->preferences.animationSpeedMultiplier, 0.25f) : 1.0f);
        if (elapsedMs >= openDurationMs) {
            state_ = GestureState::Active;
            stateEnterTicks_ = nowTicks;
            result.stateChanged = true;
        }
    } else if (state_ == GestureState::Closing) {
        const double elapsedMs = clock_->TicksToMilliseconds(nowTicks - stateEnterTicks_);
        const double closeDurationMs = (lastCancelReason_ == CancelReason::None) ? 90.0 : 80.0;
        if (elapsedMs >= closeDurationMs) {
            state_ = GestureState::Idle;
            stateEnterTicks_ = nowTicks;
            result.stateChanged = true;
        }
    }

    // Check 250ms hover dwell for child-ring sector while Opening or Active
    if ((state_ == GestureState::Opening || state_ == GestureState::Active) &&
        !inChildRing_ && dwellingSector_ >= 0 && dwellingSector_ == selectedSector_ && activeConfig_) {
        const uint32_t dwellMs = activeConfig_->preferences.childRingDwellMs;
        const double elapsedMs = clock_->TicksToMilliseconds(nowTicks - dwellStartTicks_);
        if (elapsedMs >= static_cast<double>(dwellMs)) {
            if (const RadialRing* ring = GetActiveRing()) {
                if (static_cast<size_t>(dwellingSector_) < ring->sectorActionIds.size()) {
                    const auto* act = activeConfig_->FindAction(ring->sectorActionIds[static_cast<size_t>(dwellingSector_)]);
                    if (act && act->enabled && act->type == ActionType::OpenChildRing) {
                        if (const RadialRing* childRing = activeConfig_->FindRing(act->childRingId)) {
                            inChildRing_ = true;
                            activeRingId_ = childRing->id;
                            dwellingSector_ = -1;
                            RebuildLayoutForActiveRing(currentLayout_.anchorScreenPx, currentLayout_.monitorWorkAreaPx, currentLayout_.dpi);
                            UpdateSelectionFromCursor(latestCursorPx_, nowTicks, result);
                            result.navigatedChildRing = true;
                            result.selectionChanged = true;
                        }
                    }
                }
            }
        }
    }

    // Check deferred SendShortcut timeout
    if (pendingState_ == PendingActionState::WaitingForModifierRelease && pendingShortcut_.has_value()) {
        const double waitedMs = clock_->TicksToMilliseconds(nowTicks - pendingShortcutQueuedTicks_);
        if (waitedMs > 1500.0) {
            pendingState_ = PendingActionState::Cancelled;
            lastCancelReason_ = CancelReason::ShortcutModifierReleaseTimeout;
            result.cancelReason = CancelReason::ShortcutModifierReleaseTimeout;
            pendingShortcut_.reset();
        } else if (!AreAnyActivationModifiersDown()) {
            if (pendingShortcut_->originalForeground != nullptr &&
                currentForegroundHwnd != nullptr &&
                currentForegroundHwnd != pendingShortcut_->originalForeground) {
                pendingState_ = PendingActionState::Cancelled;
                lastCancelReason_ = CancelReason::ForegroundChangedBeforeShortcut;
                result.cancelReason = CancelReason::ForegroundChangedBeforeShortcut;
                pendingShortcut_.reset();
            } else {
                pendingState_ = PendingActionState::ReadyToInject;
                result.readyDeferredCommit = std::move(*pendingShortcut_);
                pendingShortcut_.reset();
                pendingState_ = PendingActionState::Dispatched;
            }
        }
    }

    return result;
}

GestureStepResult GestureStateMachine::OnSessionOrDesktopLost() {
    GestureStepResult result;
    const int64_t nowTicks = clock_->NowTicks();
    leftWinDown_ = false;
    rightWinDown_ = false;
    capsDown_ = false;
    ctrlDown_ = false;
    altDown_ = false;
    shiftDown_ = false;
    capsEnteredBeforeWin_ = false;
    capsOwnedByGesture_ = false;
    consumeMatchingRightUp_ = false;
    consumeMatchingLeftUp_ = false;

    if (pendingState_ == PendingActionState::WaitingForModifierRelease) {
        pendingState_ = PendingActionState::Cancelled;
        pendingShortcut_.reset();
    }

    if (state_ == GestureState::Opening || state_ == GestureState::Active || state_ == GestureState::Closing) {
        state_ = GestureState::Idle;
        stateEnterTicks_ = nowTicks;
        selectedSector_ = -1;
        dwellingSector_ = -1;
        inChildRing_ = false;
        lastCancelReason_ = CancelReason::SessionLockedOrDesktopLost;
        result.cancelReason = CancelReason::SessionLockedOrDesktopLost;
        result.stateChanged = true;
    }
    return result;
}

GestureStepResult GestureStateMachine::OnQueueOverflow() {
    GestureStepResult result;
    const int64_t nowTicks = clock_->NowTicks();
    if (pendingState_ == PendingActionState::WaitingForModifierRelease) {
        pendingState_ = PendingActionState::Cancelled;
        pendingShortcut_.reset();
    }
    CancelActiveGesture(CancelReason::QueueOverflow, nowTicks, result);
    state_ = GestureState::Idle;
    result.stateChanged = true;
    return result;
}

void GestureStateMachine::SetSuspended(bool suspended) {
    if (suspended) {
        if (pendingState_ == PendingActionState::WaitingForModifierRelease) {
            pendingState_ = PendingActionState::Cancelled;
            pendingShortcut_.reset();
        }
        state_ = GestureState::Suspended;
        selectedSector_ = -1;
        dwellingSector_ = -1;
        inChildRing_ = false;
        lastCancelReason_ = CancelReason::SuspendedOrShuttingDown;
    } else if (state_ == GestureState::Suspended) {
        ResetAllState();
    }
}

void GestureStateMachine::ResetAllState() {
    state_ = GestureState::Idle;
    leftWinDown_ = false;
    rightWinDown_ = false;
    capsDown_ = false;
    ctrlDown_ = false;
    altDown_ = false;
    shiftDown_ = false;
    capsEnteredBeforeWin_ = false;
    capsOwnedByGesture_ = false;
    consumeMatchingRightUp_ = false;
    consumeMatchingLeftUp_ = false;
    inChildRing_ = false;
    isToggleSession_ = false;
    selectedSector_ = -1;
    dwellingSector_ = -1;
    if (pendingState_ == PendingActionState::WaitingForModifierRelease) {
        pendingState_ = PendingActionState::Cancelled;
        pendingShortcut_.reset();
    }
}

} // namespace keychain
