#include "input/input_manager.h"

namespace keychain {

namespace {
constexpr UINT kMsgResetHooks       = WM_APP + 10;
constexpr UINT kMsgUpdateHotKey     = WM_APP + 11;
constexpr UINT kMsgEmitStartMask    = WM_APP + 12;
constexpr int  kHotKeyCtrlAltSpace  = 0x4B01;
} // namespace

InputManager* InputManager::s_instance_ = nullptr;

InputManager::InputManager(HANDLE consumerWakeEvent)
    : consumerWakeEvent_(consumerWakeEvent) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = (freq.QuadPart > 0) ? freq.QuadPart : 10000000LL;
    threadReadyEvent_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    resetDoneEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    s_instance_ = this;
}

InputManager::~InputManager() {
    Stop();
    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
}

bool InputManager::Start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    ResetEvent(threadReadyEvent_.Get());
    running_.store(true, std::memory_order_release);
    hookThread_ = std::thread(&InputManager::HookThreadMain, this);
    WaitForSingleObject(threadReadyEvent_.Get(), 3000);
    return keyboardHook_ != nullptr && mouseHook_ != nullptr;
}

void InputManager::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (hookThreadId_ != 0) {
        PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0);
    }
    if (hookThread_.joinable()) {
        hookThread_.join();
    }
    hookThreadId_ = 0;
}

bool InputManager::ResetAndReinstallHooks() {
    if (!running_.load(std::memory_order_acquire) || hookMsgHwnd_ == nullptr) {
        return false;
    }
    ResetEvent(resetDoneEvent_.Get());
    PostMessageW(hookMsgHwnd_, kMsgResetHooks, 0, 0);
    WaitForSingleObject(resetDoneEvent_.Get(), 2000);
    return keyboardHook_ != nullptr && mouseHook_ != nullptr;
}

void InputManager::SetConfigurationMode(bool enabled, bool useWinCaps, bool useCtrlAltSpace, bool toggleMode) {
    enabled_.store(enabled, std::memory_order_release);
    useWinCaps_.store(useWinCaps, std::memory_order_release);
    useCtrlAltSpace_.store(useCtrlAltSpace, std::memory_order_release);
    toggleMode_.store(toggleMode, std::memory_order_release);

    if (hookMsgHwnd_ != nullptr) {
        PostMessageW(hookMsgHwnd_, kMsgUpdateHotKey, 0, 0);
    }
}

void InputManager::NotifyGestureActiveState(bool gestureActive) noexcept {
    gestureActive_.store(gestureActive, std::memory_order_release);
}

bool InputManager::CheckCtrlAltSpaceAvailability(std::string& outConflictGuidance) {
    if (ctrlAltSpaceRegistered_.load(std::memory_order_acquire)) {
        outConflictGuidance.clear();
        return true;
    }
    // Probe registration on the current thread with a temporary test hotkey ID
    if (RegisterHotKey(nullptr, 0x4B99, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE)) {
        UnregisterHotKey(nullptr, 0x4B99);
        ctrlAltSpaceAvailable_.store(true, std::memory_order_release);
        outConflictGuidance.clear();
        return true;
    }
    ctrlAltSpaceAvailable_.store(false, std::memory_order_release);
    outConflictGuidance =
        "Ctrl+Alt+Space is currently registered by another running application. "
        "Close or reconfigure the conflicting shortcut utility, or use Win + Caps Lock.";
    return false;
}

void InputManager::EmitStartMenuMaskSequence() noexcept {
    // Narrowly scoped, self-tagged non-printable key press+release (VK_NONAME / 0xFC)
    // while Win is held during a recognized Win+Caps gesture. Prevents Explorer from
    // opening the Start menu when the user later releases the Win key.
    INPUT inputs[2]{};
    inputs[0].type           = INPUT_KEYBOARD;
    inputs[0].ki.wVk         = 0xFF; // Unassigned mask virtual key
    inputs[0].ki.dwFlags     = 0;
    inputs[0].ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;

    inputs[1].type           = INPUT_KEYBOARD;
    inputs[1].ki.wVk         = 0xFF;
    inputs[1].ki.dwFlags     = KEYEVENTF_KEYUP;
    inputs[1].ki.dwExtraInfo = kKeychainSelfGeneratedInputTag;

    SendInput(2, inputs, sizeof(INPUT));
}

InputHookStatus InputManager::GetStatus() const {
    InputHookStatus st;
    st.keyboardHookInstalled  = (keyboardHook_ != nullptr);
    st.mouseHookInstalled     = (mouseHook_ != nullptr);
    st.ctrlAltSpaceRegistered = ctrlAltSpaceRegistered_.load(std::memory_order_acquire);
    st.ctrlAltSpaceAvailable  = ctrlAltSpaceAvailable_.load(std::memory_order_acquire);
    st.reinstallCount         = reinstallCount_.load(std::memory_order_acquire);
    if (!st.ctrlAltSpaceAvailable) {
        st.conflictGuidance =
            "Ctrl+Alt+Space conflict detected. Another application owns this global hotkey. "
            "Use Win + Caps Lock or release the shortcut in the conflicting app.";
    }
    return st;
}

void InputManager::InstallHooksOnThread() {
    UninstallHooksOnThread();

    hkLeftWinDown_            = false;
    hkRightWinDown_           = false;
    hkCapsDown_               = false;
    hkCtrlDown_               = false;
    hkAltDown_                = false;
    hkShiftDown_              = false;
    hkCapsEnteredBeforeWin_   = false;
    hkCapsOwnedByGesture_     = false;
    hkConsumeMatchingRightUp_ = false;
    hkConsumeMatchingLeftUp_  = false;

    HINSTANCE hMod = GetModuleHandleW(nullptr);
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputManager::LowLevelKeyboardProc, hMod, 0);
    mouseHook_    = SetWindowsHookExW(WH_MOUSE_LL, &InputManager::LowLevelMouseProc, hMod, 0);
    reinstallCount_.fetch_add(1, std::memory_order_acq_rel);
}

void InputManager::UninstallHooksOnThread() {
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void InputManager::UpdateHotKeyRegistrationOnThread() {
    if (!hookMsgHwnd_) return;

    if (ctrlAltSpaceRegistered_.load(std::memory_order_acquire)) {
        UnregisterHotKey(hookMsgHwnd_, kHotKeyCtrlAltSpace);
        ctrlAltSpaceRegistered_.store(false, std::memory_order_release);
    }

    if (enabled_.load(std::memory_order_acquire) && useCtrlAltSpace_.load(std::memory_order_acquire)) {
        if (RegisterHotKey(hookMsgHwnd_, kHotKeyCtrlAltSpace, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE)) {
            ctrlAltSpaceRegistered_.store(true, std::memory_order_release);
            ctrlAltSpaceAvailable_.store(true, std::memory_order_release);
        } else {
            ctrlAltSpaceRegistered_.store(false, std::memory_order_release);
            ctrlAltSpaceAvailable_.store(false, std::memory_order_release);
        }
    }
}

LRESULT CALLBACK InputManager::HookWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    InputManager* self = s_instance_;
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case kMsgResetHooks:
            self->queue_.Clear();
            self->InstallHooksOnThread();
            self->UpdateHotKeyRegistrationOnThread();
            SetEvent(self->resetDoneEvent_.Get());
            return 0;

        case kMsgUpdateHotKey:
            self->UpdateHotKeyRegistrationOnThread();
            return 0;

        case kMsgEmitStartMask:
            EmitStartMenuMaskSequence();
            return 0;

        case WM_HOTKEY:
            if (wParam == kHotKeyCtrlAltSpace && self->enabled_.load(std::memory_order_acquire)) {
                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);
                POINT pt{};
                GetCursorPos(&pt);

                InputQueueEvent ev{};
                ev.type           = InputEventType::HotKeyChord;
                ev.cursorScreenPx = Point2I{pt.x, pt.y};
                ev.receiptQpc     = qpc.QuadPart;

                self->gestureActive_.store(true, std::memory_order_release);
                self->queue_.Push(ev);
                if (self->consumerWakeEvent_) {
                    SetEvent(self->consumerWakeEvent_);
                }
            }
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void InputManager::HookThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"KeychainInputHookThread");
    hookThreadId_ = GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &InputManager::HookWindowProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeychainInputHookMessageWindow";
    RegisterClassExW(&wc);

    hookMsgHwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"KeychainInputHook",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        wc.hInstance,
        nullptr
    );

    InstallHooksOnThread();
    UpdateHotKeyRegistrationOnThread();
    SetEvent(threadReadyEvent_.Get());

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ctrlAltSpaceRegistered_.load(std::memory_order_acquire) && hookMsgHwnd_) {
        UnregisterHotKey(hookMsgHwnd_, kHotKeyCtrlAltSpace);
        ctrlAltSpaceRegistered_.store(false, std::memory_order_release);
    }
    UninstallHooksOnThread();

    if (hookMsgHwnd_) {
        DestroyWindow(hookMsgHwnd_);
        hookMsgHwnd_ = nullptr;
    }
}

LRESULT CALLBACK InputManager::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
    if (nCode < 0 || lParam == 0) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    InputManager* self = s_instance_;
    if (!self) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    // Ignore self-generated input (Start-menu mask or SendShortcut injection)
    if (kb->dwExtraInfo == kKeychainSelfGeneratedInputTag) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const uint32_t vk = kb->vkCode;
    bool isRepeat = false;

    // Maintain modifier state strictly from arriving events (no GetAsyncKeyState)
    if (vk == VK_LWIN) {
        isRepeat = (self->hkLeftWinDown_ && isDown);
        self->hkLeftWinDown_ = isDown;
    } else if (vk == VK_RWIN) {
        isRepeat = (self->hkRightWinDown_ && isDown);
        self->hkRightWinDown_ = isDown;
    } else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        self->hkCtrlDown_ = isDown;
    } else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        self->hkAltDown_ = isDown;
    } else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        self->hkShiftDown_ = isDown;
    } else if (vk == VK_CAPITAL) {
        isRepeat = (self->hkCapsDown_ && isDown);
    }

    const bool anyWinDown     = self->hkLeftWinDown_ || self->hkRightWinDown_;
    const bool enabled        = self->enabled_.load(std::memory_order_relaxed);
    const bool useWinCaps     = self->useWinCaps_.load(std::memory_order_relaxed);
    const bool gestureActive  = self->gestureActive_.load(std::memory_order_relaxed);

    bool suppressEvent = false;
    bool RelevantForEngine = false;

    if (vk == VK_CAPITAL) {
        if (isDown) {
            if (self->hkCapsOwnedByGesture_) {
                // Suppress key repeat while gesture owns Caps Lock
                suppressEvent     = true;
                RelevantForEngine = true;
            } else if (!anyWinDown || self->hkCapsEnteredBeforeWin_) {
                // Caps Lock pressed before Win: retain ordinary Caps Lock behavior!
                self->hkCapsDown_             = true;
                self->hkCapsEnteredBeforeWin_ = true;
                suppressEvent                 = false;
                RelevantForEngine             = true;
            } else if (enabled && useWinCaps && !self->hkCtrlDown_ && !self->hkAltDown_) {
                // Recognized Win-down followed by Caps-down!
                self->hkCapsDown_           = true;
                self->hkCapsOwnedByGesture_ = true;
                self->gestureActive_.store(true, std::memory_order_relaxed);
                suppressEvent               = true;
                RelevantForEngine           = true;

                // Schedule narrowly scoped Start-menu mask sequence
                if (self->hookMsgHwnd_) {
                    PostMessageW(self->hookMsgHwnd_, kMsgEmitStartMask, 0, 0);
                }
            } else {
                self->hkCapsDown_ = true;
                suppressEvent     = false;
            }
        } else {
            // Caps Lock key-up
            self->hkCapsDown_             = false;
            self->hkCapsEnteredBeforeWin_ = false;
            if (self->hkCapsOwnedByGesture_) {
                self->hkCapsOwnedByGesture_ = false;
                suppressEvent               = true;
                RelevantForEngine           = true;
            } else {
                RelevantForEngine = true;
            }
        }
    } else if (vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
               vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        // Forward modifier events to OS (never swallow Win-down), but notify engine
        RelevantForEngine = true;
        if (!anyWinDown && !isDown && gestureActive && !self->toggleMode_.load(std::memory_order_relaxed)) {
            self->gestureActive_.store(false, std::memory_order_relaxed);
        }
    } else if (gestureActive) {
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE ||
            vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
            vk == VK_TAB || vk == VK_BACK || (vk >= '1' && vk <= '8')) {
            if (isDown) {
                suppressEvent = true;
            }
            RelevantForEngine = true;
        }
    }

    if (RelevantForEngine) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        POINT pt{};
        GetCursorPos(&pt);

        InputQueueEvent ev{};
        ev.type                       = InputEventType::KeyEvent;
        ev.vkCode                     = vk;
        ev.isDown                     = isDown;
        ev.isRepeat                   = isRepeat;
        ev.capsToggleStateBeforeEvent = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        ev.cursorScreenPx             = Point2I{pt.x, pt.y};
        ev.receiptQpc                 = qpc.QuadPart;

        self->queue_.Push(ev);
        if (self->consumerWakeEvent_) {
            SetEvent(self->consumerWakeEvent_);
        }
    }

    if (suppressEvent) {
        return 1;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK InputManager::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
    if (nCode < 0 || lParam == 0) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    InputManager* self = s_instance_;
    if (!self) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const bool gestureActive = self->gestureActive_.load(std::memory_order_relaxed);

    // Outside active gestures, preserve 100% normal mouse behavior unless consuming a matching button-up
    if (!gestureActive && !self->hkConsumeMatchingRightUp_ && !self->hkConsumeMatchingLeftUp_) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    if (wParam == WM_MOUSEMOVE) {
        if (gestureActive) {
            InputQueueEvent ev{};
            ev.type           = InputEventType::PointerMove;
            ev.cursorScreenPx = Point2I{ms->pt.x, ms->pt.y};
            ev.receiptQpc     = qpc.QuadPart;
            self->queue_.Push(ev);
            if (self->consumerWakeEvent_) {
                SetEvent(self->consumerWakeEvent_);
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (wParam == WM_RBUTTONDOWN && gestureActive) {
        self->hkConsumeMatchingRightUp_ = true;
        InputQueueEvent ev{};
        ev.type               = InputEventType::MouseButton;
        ev.isRightMouseButton = true;
        ev.isDown             = true;
        ev.cursorScreenPx     = Point2I{ms->pt.x, ms->pt.y};
        ev.receiptQpc         = qpc.QuadPart;
        self->queue_.Push(ev);
        if (self->consumerWakeEvent_) {
            SetEvent(self->consumerWakeEvent_);
        }
        return 1; // Suppress right-button down during active gesture
    }

    if (wParam == WM_RBUTTONUP && (gestureActive || self->hkConsumeMatchingRightUp_)) {
        const bool shouldConsume = self->hkConsumeMatchingRightUp_;
        self->hkConsumeMatchingRightUp_ = false;
        InputQueueEvent ev{};
        ev.type               = InputEventType::MouseButton;
        ev.isRightMouseButton = true;
        ev.isDown             = false;
        ev.cursorScreenPx     = Point2I{ms->pt.x, ms->pt.y};
        ev.receiptQpc         = qpc.QuadPart;
        self->queue_.Push(ev);
        if (self->consumerWakeEvent_) {
            SetEvent(self->consumerWakeEvent_);
        }
        if (shouldConsume) {
            return 1; // Suppress matching right-button up so underlying context menu never pops
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (wParam == WM_LBUTTONDOWN && gestureActive) {
        self->hkConsumeMatchingLeftUp_ = true;
        InputQueueEvent ev{};
        ev.type               = InputEventType::MouseButton;
        ev.isRightMouseButton = false;
        ev.isDown             = true;
        ev.cursorScreenPx     = Point2I{ms->pt.x, ms->pt.y};
        ev.receiptQpc         = qpc.QuadPart;
        self->queue_.Push(ev);
        if (self->consumerWakeEvent_) {
            SetEvent(self->consumerWakeEvent_);
        }
        return 1;
    }

    if (wParam == WM_LBUTTONUP && (gestureActive || self->hkConsumeMatchingLeftUp_)) {
        const bool shouldConsume = self->hkConsumeMatchingLeftUp_;
        self->hkConsumeMatchingLeftUp_ = false;
        InputQueueEvent ev{};
        ev.type               = InputEventType::MouseButton;
        ev.isRightMouseButton = false;
        ev.isDown             = false;
        ev.cursorScreenPx     = Point2I{ms->pt.x, ms->pt.y};
        ev.receiptQpc         = qpc.QuadPart;
        self->queue_.Push(ev);
        if (self->consumerWakeEvent_) {
            SetEvent(self->consumerWakeEvent_);
        }
        if (shouldConsume) {
            return 1;
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace keychain
