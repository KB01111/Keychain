#pragma once

#include "animation/spring_animator.h"
#include "common/types.h"
#include "config/config_schema.h"
#include "diagnostics/diagnostics_manager.h"
#include "graphics/text_icon_atlas.h"
#include "layout/radial_layout.h"

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace keychain {

#pragma warning(push)
#pragma warning(disable : 4324)

// Must match cbuffer RingConstants : register(b0) in src/shaders/ring_sdf.hlsl (256-byte aligned)
struct alignas(256) RingConstantsGpu {
    float surfaceSizePx[2];
    float ringCenterPx[2];

    float innerRadiusPx;
    float outerRadiusPx;
    float hubRadiusPx;
    float sectorGapRad;

    float startAngleRad;
    float arcSpanRad;
    uint32_t sectorCount;
    int32_t selectedSector;

    float openScale;
    float ringOpacity;
    float selectionAlpha;
    float highlightAngleRad;

    float selectionLiftPx;
    float confirmPulse;
    float dwellProgress;
    uint32_t flags;

    uint32_t sectorStateMask;
    uint32_t childRingSectorMask;
    float shadowRadiusPx;
    float rimIntensity;

    float glassTint[4];
    float glassHighlight[4];
    float accentColor[4];
    float rimColor[4];
    float shadowColor[4];
    float disabledTint[4];
};

// Must match cbuffer AtlasConstants : register(b0) in src/shaders/atlas_quad.hlsl
struct alignas(256) AtlasConstantsGpu {
    float surfaceSizePx[2];
    float ringCenterPx[2];
    float openScale;
    float ringOpacity;
    float pad0;
    float pad1;
};

#pragma warning(pop)

struct OverlayRenderParams {
    RadialLayout layout{};
    RingVisualState visual{};
    const ConfigDocument* config = nullptr;
    std::string activeRingId;
    int selectedSector = -1;
    float childRingDwellProgress = 0.0f;
    bool isChildRing = false;
    bool isPreviewMode = false;
    bool systemHighContrast = false;
    bool systemReducedTransparency = false;
    bool systemLightMode = false;
    std::vector<bool> sectorAvailable;
};

struct FrameTimingResult {
    bool rendered = false;
    bool deviceLost = false;
    double cpuWorkMs = 0.0;
    double previousFrameGpuWorkMs = 0.0;
    int64_t frameSubmittedQpcUs = 0;
};

// [Section 5 & 6] Concrete D3D12 + DirectComposition overlay renderer.
class D3D12OverlayRenderer {
public:
    static constexpr uint32_t kBufferCount = 2;
    static constexpr uint32_t kMaxQuadsPerFrame = 512;

    D3D12OverlayRenderer(IClock& clock, DiagnosticsManager& diagnostics);
    ~D3D12OverlayRenderer();

    D3D12OverlayRenderer(const D3D12OverlayRenderer&) = delete;
    D3D12OverlayRenderer& operator=(const D3D12OverlayRenderer&) = delete;

    // Initializes hardware adapter, D3D12 device, command queue, swap chain for composition,
    // DirectComposition visual tree, PSOs, and prewarms a valid transparent frame.
    bool Initialize(HWND overlayHwnd, uint32_t widthPx, uint32_t heightPx, const std::wstring& preferredAdapterName = L"");
    void Shutdown();

    bool ResizeIfNeeded(uint32_t widthPx, uint32_t heightPx);

    // Pre-caches icons and text outside the activation critical path
    void PrecacheConfigAssets(const ConfigDocument& config, uint32_t dpi);

    // Renders a single premultiplied-alpha frame (or transparent clear frame)
    FrameTimingResult RenderFrame(const OverlayRenderParams& params);
    bool PrewarmTransparentFrame();

    // Reads back the current back-buffer pixels into BGRA8 for deterministic visual QA / screenshot verification
    bool CaptureCurrentBackBufferToPng(const std::wstring& outputPath);

    // [Section 6, R7] Compositor clock integration + DRR boost paired request/release
    void SetCompositorClockBoost(bool enable);
    DWORD WaitForNextFrameOrWake(HANDLE wakeEvent, DWORD timeoutMs);

    void WaitForGpuIdle();

    bool IsInitialized() const noexcept { return initialized_; }
    bool IsPermanentlyFailed() const noexcept { return recoveryAttempts_ >= 3; }
    std::wstring SelectedAdapterName() const { return selectedAdapterName_; }
    std::string SelectedAdapterLuid() const noexcept { return selectedAdapterLuid_; }
    bool HasCompositorClockSupport() const noexcept { return pfnWaitForCompositorClock_ != nullptr; }
    std::vector<std::wstring> EnumerateHardwareAdapters() const;
    std::vector<AdapterDiagnosticInfo> GetAdapterDiagnostics() const;

private:
    struct FrameContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        uint8_t* mappedUploadPtr = nullptr;
        uint64_t fenceValue = 0;
        bool hasTimestampQuery = false;
    };

    bool SelectHardwareAdapter(const std::wstring& preferredName, IDXGIAdapter1** outAdapter);
    bool CreateDeviceAndCommandObjects(IDXGIAdapter1* adapter);
    bool CreateCompositionSwapChainAndVisual(HWND hwnd, uint32_t widthPx, uint32_t heightPx);
    bool CreatePipelineStatesAndHeaps();
    bool CreateAtlasTextures();
    bool CreateRenderTargetViews();
    void UploadDirtyAtlases(ID3D12GraphicsCommandList* cmdList);
    bool TryRecoverFromDeviceLoss();

    IClock& clock_;
    DiagnosticsManager& diagnostics_;

    HWND overlayHwnd_ = nullptr;
    uint32_t widthPx_ = 540;
    uint32_t heightPx_ = 540;
    std::wstring preferredAdapterName_;
    std::wstring selectedAdapterName_;
    std::string selectedAdapterLuid_;
    bool initialized_ = false;
    bool shutdownCompleted_ = false;
    uint32_t recoveryAttempts_ = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    UniqueHandle frameLatencyWaitableHandle_;
    bool waitableSwapChain_ = false;

    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> dcompVisual_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    uint32_t rtvDescriptorSize_ = 0;
    uint32_t srvDescriptorSize_ = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> ringRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ringPso_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> atlasRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> atlasPso_;

    std::array<FrameContext, kBufferCount> frames_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UniqueHandle fenceEvent_;
    uint64_t nextFenceValue_ = 1;

    // GPU timestamp queries (non-blocking 1-cycle delayed readback)
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampQueryHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadbackBuffer_;
    uint64_t gpuTimestampFrequency_ = 0;

    // Atlas GPU textures + staging upload buffers
    TextAndIconAtlas atlas_;
    Microsoft::WRL::ComPtr<ID3D12Resource> glyphTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> glyphUploadBuffer_;
    D3D12_RESOURCE_STATES glyphTextureState_ = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> iconTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> iconUploadBuffer_;
    D3D12_RESOURCE_STATES iconTextureState_ = D3D12_RESOURCE_STATE_COMMON;

    // Dynamic compositor clock pointers from dcomp.dll [R7]
    using PfnWaitForCompositorClock = DWORD(WINAPI*)(UINT count, const HANDLE* handles, DWORD timeoutInMs);
    using PfnBoostCompositorClock = HRESULT(WINAPI*)(BOOL enable);
    PfnWaitForCompositorClock pfnWaitForCompositorClock_ = nullptr;
    PfnBoostCompositorClock pfnBoostCompositorClock_ = nullptr;
    bool compositorBoostActive_ = false;
};

} // namespace keychain
