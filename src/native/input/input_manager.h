#pragma once

#include "common/types.h"
#include <atomic>
#include <thread>
#include <array>
#include <optional>
#include <string>

namespace keychain {

constexpr ULONG_PTR kKeychainSelfGeneratedInputTag = 0x4B43484EULL; // 'KCHN'

enum class InputEventType : uint8_t {
    None = 0,
    KeyEvent,
    PointerMove,
    MouseButton,
    HotKeyChord,
    QueueOverflow
};

struct InputQueueEvent {
    InputEventType type                       = InputEventType::None;
    uint32_t       vkCode                     = 0;
    bool           isDown                     = false;
    bool           isRepeat                   = false;
    bool           isRightMouseButton         = false;
    bool           capsToggleStateBeforeEvent = false;
    Point2I        cursorScreenPx             = {};
    int64_t        receiptQpc                 = 0;
};

// Allocation-free, lock-free/spinlock-free bounded queue with tail coalescing for PointerMove
template <size_t Capacity = 256>
class BoundedInputQueue {
public:
    BoundedInputQueue() noexcept {
        InitializeSRWLock(&lock_);
    }

    // Returns false if queue overflowed on a critical (non-PointerMove) transition
    bool Push(const InputQueueEvent& ev) noexcept {
        AcquireSRWLockExclusive(&lock_);
        if (ev.type == InputEventType::PointerMove && count_ > 0) {
            const size_t lastIdx = (head_ + count_ - 1) % Capacity;
            if (buffer_[lastIdx].type == InputEventType::PointerMove) {
                // Coalesce consecutive pointer moves without losing latest position
                buffer_[lastIdx].cursorScreenPx = ev.cursorScreenPx;
                buffer_[lastIdx].receiptQpc     = ev.receiptQpc;
                ReleaseSRWLockExclusive(&lock_);
                return true;
            }
        }

        if (count_ >= Capacity) {
            if (ev.type == InputEventType::PointerMove) {
                // Drop excess pointer move rather than overflowing commit/cancel events
                ReleaseSRWLockExclusive(&lock_);
                return true;
            }
            overflowed_ = true;
            ReleaseSRWLockExclusive(&lock_);
            return false;
        }

        const size_t tail = (head_ + count_) % Capacity;
        buffer_[tail] = ev;
        ++count_;
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }

    bool Pop(InputQueueEvent& outEv) noexcept {
        AcquireSRWLockExclusive(&lock_);
        if (overflowed_) {
            overflowed_ = false;
            head_  = 0;
            count_ = 0;
            outEv  = InputQueueEvent{};
            outEv.type = InputEventType::QueueOverflow;
            ReleaseSRWLockExclusive(&lock_);
            return true;
        }
        if (count_ == 0) {
            ReleaseSRWLockExclusive(&lock_);
            return false;
        }
        outEv = buffer_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }

    void Clear() noexcept {
        AcquireSRWLockExclusive(&lock_);
        head_       = 0;
        count_      = 0;
        overflowed_ = false;
        ReleaseSRWLockExclusive(&lock_);
    }

    size_t Size() const noexcept {
        AcquireSRWLockShared(&lock_);
        size_t c = count_;
        ReleaseSRWLockShared(&lock_);
        return c;
    }

private:
    mutable SRWLOCK lock_{};
    std::array<InputQueueEvent, Capacity> buffer_{};
    size_t head_       = 0;
    size_t count_      = 0;
    bool   overflowed_ = false;
};

struct InputHookStatus {
    bool        keyboardHookInstalled      = false;
    bool        mouseHookInstalled         = false;
    bool        ctrlAltSpaceAvailable      = true;
    bool        ctrlAltSpaceRegistered     = false;
    uint32_t    reinstallCount             = 0;
    std::string conflictGuidance;
};

class InputManager {
public:
    explicit InputManager(HANDLE consumerWakeEvent);
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    bool Start();
    void Stop();

    // Real hook reset and reinstallation on the dedicated hook thread
    bool ResetAndReinstallHooks();

    // Updates atomic mode flags read by the low-level hook
    void SetConfigurationMode(bool enabled, bool useWinCaps, bool useCtrlAltSpace, bool toggleMode);
    void NotifyGestureActiveState(bool gestureActive) noexcept;

    // Tests whether Ctrl+Alt+Space can be registered or has a hotkey conflict
    bool CheckCtrlAltSpaceAvailability(std::string& outConflictGuidance);

    // Emits the narrowly scoped Start-menu mask key sequence tagged with kKeychainSelfGeneratedInputTag
    static void EmitStartMenuMaskSequence() noexcept;

    bool PopEvent(InputQueueEvent& outEvent) noexcept {
        return queue_.Pop(outEvent);
    }

    InputHookStatus GetStatus() const;

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;
    static LRESULT CALLBACK HookWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

    void HookThreadMain();
    void InstallHooksOnThread();
    void UninstallHooksOnThread();
    void UpdateHotKeyRegistrationOnThread();

    static InputManager* s_instance_;

    HANDLE consumerWakeEvent_ = nullptr;
    UniqueHandle threadReadyEvent_;
    UniqueHandle resetDoneEvent_;
    std::thread hookThread_;
    DWORD hookThreadId_ = 0;
    HWND hookMsgHwnd_ = nullptr;

    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_    = nullptr;

    BoundedInputQueue<256> queue_;

    // Atomic state shared between hook thread and consumer thread
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> useWinCaps_{true};
    std::atomic<bool> useCtrlAltSpace_{false};
    std::atomic<bool> toggleMode_{false};
    std::atomic<bool> gestureActive_{false};
    std::atomic<uint32_t> reinstallCount_{0};
    std::atomic<bool> ctrlAltSpaceRegistered_{false};
    std::atomic<bool> ctrlAltSpaceAvailable_{true};

    // Hook-thread-only tracked modifier state (never GetAsyncKeyState)
    bool hkLeftWinDown_            = false;
    bool hkRightWinDown_           = false;
    bool hkCapsDown_               = false;
    bool hkCtrlDown_               = false;
    bool hkAltDown_                = false;
    bool hkShiftDown_              = false;
    bool hkCapsEnteredBeforeWin_   = false;
    bool hkCapsOwnedByGesture_     = false;
    bool hkConsumeMatchingRightUp_ = false;
    bool hkConsumeMatchingLeftUp_  = false;
    int64_t qpcFrequency_          = 10000000LL;
};

} // namespace keychain
