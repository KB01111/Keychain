#include "app/accessibility_provider.h"

namespace keychain {

RadialOverlayAccessibilityProvider::RadialOverlayAccessibilityProvider(HWND hwnd)
    : hwnd_(hwnd) {}

HRESULT STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
        *ppvObject = static_cast<IRawElementProviderSimple*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::AddRef() {
    return InterlockedIncrement(&refCount_);
}

ULONG STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::Release() {
    const ULONG count = InterlockedDecrement(&refCount_);
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (!pRetVal) {
        return E_POINTER;
    }
    *pRetVal = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::GetPatternProvider(PATTERNID, IUnknown** pRetVal) {
    if (!pRetVal) {
        return E_POINTER;
    }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (!pRetVal) {
        return E_POINTER;
    }
    VariantInit(pRetVal);

    if (propertyId == UIA_NamePropertyId) {
        const std::wstring name = CurrentAccessibleName();
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(name.c_str());
        return S_OK;
    }
    if (propertyId == UIA_AutomationIdPropertyId) {
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"KeychainRadialOverlay");
        return S_OK;
    }
    if (propertyId == UIA_ControlTypePropertyId) {
        pRetVal->vt = VT_I4;
        pRetVal->lVal = UIA_MenuControlTypeId;
        return S_OK;
    }
    if (propertyId == UIA_LocalizedControlTypePropertyId) {
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"Radial Action Ring");
        return S_OK;
    }
    if (propertyId == UIA_IsControlElementPropertyId ||
        propertyId == UIA_IsContentElementPropertyId ||
        propertyId == UIA_IsKeyboardFocusablePropertyId) {
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = VARIANT_TRUE;
        return S_OK;
    }
    if (propertyId == UIA_HelpTextPropertyId) {
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(
            L"Use Arrow keys or Tab to select a sector, Enter to commit, Backspace to return from subring, Escape to cancel.");
        return S_OK;
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE RadialOverlayAccessibilityProvider::get_HostRawElementProvider(
    IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) {
        return E_POINTER;
    }
    if (!hwnd_ || !IsWindow(hwnd_)) {
        *pRetVal = nullptr;
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    return UiaHostProviderFromHwnd(hwnd_, pRetVal);
}

void RadialOverlayAccessibilityProvider::NotifyOverlayOpened(const std::wstring& ringTitle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ringTitle_ = ringTitle.empty() ? L"Keychain Radial Launcher" : ringTitle;
        selectedSector_ = -1;
        selectedActionLabel_ = L"Center (Cancel)";
        selectedAvailable_ = true;
        isOpen_ = true;
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowTextW(hwnd_, CurrentAccessibleName().c_str());
        NotifyWinEvent(EVENT_SYSTEM_MENUPOPUPSTART, hwnd_, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd_, OBJID_CLIENT, CHILDID_SELF);
    }
}

void RadialOverlayAccessibilityProvider::NotifySelectionChanged(
    int sectorIndex,
    const std::wstring& actionLabel,
    bool available) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (selectedSector_ != sectorIndex || selectedActionLabel_ != actionLabel || selectedAvailable_ != available) {
            selectedSector_ = sectorIndex;
            selectedActionLabel_ = actionLabel.empty() ? L"None" : actionLabel;
            selectedAvailable_ = available;
            changed = true;
        }
    }
    if (changed && hwnd_ && IsWindow(hwnd_)) {
        SetWindowTextW(hwnd_, CurrentAccessibleName().c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd_, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_SELECTION, hwnd_, OBJID_CLIENT, sectorIndex >= 0 ? (sectorIndex + 1) : CHILDID_SELF);
    }
}

void RadialOverlayAccessibilityProvider::NotifyOverlayClosed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        isOpen_ = false;
        selectedSector_ = -1;
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        NotifyWinEvent(EVENT_SYSTEM_MENUPOPUPEND, hwnd_, OBJID_CLIENT, CHILDID_SELF);
    }
}

std::wstring RadialOverlayAccessibilityProvider::CurrentAccessibleName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isOpen_) {
        return L"Keychain Radial Launcher (Hidden)";
    }
    if (selectedSector_ < 0) {
        return ringTitle_ + L" - Center (Release or Escape to Cancel)";
    }
    std::wstring status = selectedAvailable_ ? L"" : L" (Unavailable)";
    return ringTitle_ + L" - Sector " + std::to_wstring(selectedSector_ + 1) + L": " + selectedActionLabel_ + status;
}

int RadialOverlayAccessibilityProvider::CurrentSelectedSector() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selectedSector_;
}

LRESULT RadialOverlayAccessibilityProvider::HandleWmGetObject(WPARAM wParam, LPARAM lParam) {
    if (static_cast<long>(lParam) == static_cast<long>(UiaRootObjectId)) {
        return UiaReturnRawElementProvider(hwnd_, wParam, lParam, this);
    }
    return DefWindowProcW(hwnd_, WM_GETOBJECT, wParam, lParam);
}

} // namespace keychain
