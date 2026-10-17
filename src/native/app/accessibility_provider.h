#pragma once

#include <Windows.h>
#include <ole2.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#pragma warning(push)
#pragma warning(disable : 4471)
#include <UIAutomationClient.h>
#pragma warning(pop)
#include <oleacc.h>
#include <wrl/client.h>

#include <mutex>
#include <string>

namespace keychain {

// Implements a server-side UI Automation provider (IRawElementProviderSimple) and WinEvent notifier
// for the custom DX12/DirectComposition radial overlay HWND.
// Kept strictly out of the fast WH_KEYBOARD_LL callback path.
class RadialOverlayAccessibilityProvider final : public IRawElementProviderSimple {
public:
    explicit RadialOverlayAccessibilityProvider(HWND hwnd);
    ~RadialOverlayAccessibilityProvider() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override;

    // State updates called from the main/render thread (never from the low-level hook callback)
    void NotifyOverlayOpened(const std::wstring& ringTitle);
    void NotifySelectionChanged(int sectorIndex, const std::wstring& actionLabel, bool available);
    void NotifyOverlayClosed();

    std::wstring CurrentAccessibleName() const;
    int CurrentSelectedSector() const;

    LRESULT HandleWmGetObject(WPARAM wParam, LPARAM lParam);

private:
    ULONG refCount_ = 1;
    HWND hwnd_ = nullptr;
    mutable std::mutex mutex_;
    std::wstring ringTitle_ = L"Keychain Radial Launcher";
    std::wstring selectedActionLabel_ = L"None";
    int selectedSector_ = -1;
    bool selectedAvailable_ = true;
    bool isOpen_ = false;
};

} // namespace keychain
