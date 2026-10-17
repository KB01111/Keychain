#include "graphics/d3d12_overlay_renderer.h"

#include "graphics/atlas_quad_ps_bytecode.h"
#include "graphics/atlas_quad_vs_bytecode.h"
#include "graphics/ring_sdf_ps_bytecode.h"
#include "graphics/ring_sdf_vs_bytecode.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace keychain {

namespace {

constexpr size_t kUploadBufferSizeBytes = 64 * 1024; // 64 KiB per frame context
constexpr size_t kRingConstantsOffset = 0;
constexpr size_t kAtlasConstantsOffset = 256;
constexpr size_t kQuadVerticesOffset = 512;

void WriteColor4(float dst[4], const Color4F& c) {
    dst[0] = c.r;
    dst[1] = c.g;
    dst[2] = c.b;
    dst[3] = c.a;
}

} // namespace

D3D12OverlayRenderer::D3D12OverlayRenderer(IClock& clock, DiagnosticsManager& diagnostics)
    : clock_(clock)
    , diagnostics_(diagnostics) {
    HMODULE hDcomp = GetModuleHandleW(L"dcomp.dll");
    if (!hDcomp) {
        hDcomp = LoadLibraryW(L"dcomp.dll");
    }
    if (hDcomp) {
        pfnWaitForCompositorClock_ = reinterpret_cast<PfnWaitForCompositorClock>(
            GetProcAddress(hDcomp, "DCompositionWaitForCompositorClock"));
        pfnBoostCompositorClock_ = reinterpret_cast<PfnBoostCompositorClock>(
            GetProcAddress(hDcomp, "DCompositionBoostCompositorClock"));
    }
}

D3D12OverlayRenderer::~D3D12OverlayRenderer() {
    Shutdown();
}

std::vector<std::wstring> D3D12OverlayRenderer::EnumerateHardwareAdapters() const {
    std::vector<std::wstring> names;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return names;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                names.emplace_back(desc.Description);
            }
        }
        adapter.Reset();
    }
    return names;
}

namespace {

std::string AdapterLuidToHexString(const LUID& luid) noexcept {
    static const char* kHexDigits = "0123456789ABCDEF";
    std::string hex(16, '0');
    const uint32_t parts[2] = {static_cast<uint32_t>(luid.HighPart), static_cast<uint32_t>(luid.LowPart)};
    for (int p = 0; p < 2; ++p) {
        for (int nibble = 0; nibble < 8; ++nibble) {
            const uint32_t shift = static_cast<uint32_t>(28 - 4 * nibble);
            const uint32_t digit = (parts[p] >> shift) & 0xFu;
            hex[static_cast<size_t>(p * 8 + nibble)] = kHexDigits[digit];
        }
    }
    return hex;
}

bool HexEqualsIgnoreCase(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'F') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Formats an HRESULT as "0x........" for diagnostics logging.
std::string HrHex(HRESULT hr) {
    static const char kHexDigits[] = "0123456789ABCDEF";
    const unsigned int value = static_cast<unsigned int>(hr);
    std::string s = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        s += kHexDigits[(value >> shift) & 0xFu];
    }
    return s;
}

} // namespace

std::vector<AdapterDiagnosticInfo> D3D12OverlayRenderer::GetAdapterDiagnostics() const {
    std::vector<AdapterDiagnosticInfo> infos;

    IDXGIFactory6* factory = dxgiFactory_.Get();
    Microsoft::WRL::ComPtr<IDXGIFactory6> localFactory;
    if (!factory) {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&localFactory)))) {
            return infos;
        }
        factory = localFactory.Get();
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            AdapterDiagnosticInfo info;
            info.description = WideToUtf8(desc.Description);
            info.luidHex = AdapterLuidToHexString(desc.AdapterLuid);
            info.dedicatedVideoMemoryMb = desc.DedicatedVideoMemory / (1024ull * 1024ull);
            info.isDiscrete = desc.DedicatedVideoMemory > (512ull * 1024ull * 1024ull);
            info.isSelected = (!selectedAdapterLuid_.empty() && info.luidHex == selectedAdapterLuid_);
            infos.push_back(std::move(info));
        }
        adapter.Reset();
    }
    return infos;
}

bool D3D12OverlayRenderer::Initialize(
    HWND overlayHwnd,
    uint32_t widthPx,
    uint32_t heightPx,
    const std::wstring& preferredAdapterName) {
    overlayHwnd_ = overlayHwnd;
    widthPx_ = std::clamp(widthPx, 128u, 2048u);
    heightPx_ = std::clamp(heightPx, 128u, 2048u);
    preferredAdapterName_ = preferredAdapterName;

    UINT dxgiFlags = 0;
#if defined(_DEBUG)
    // Enable D3D12 debug layer in development builds, not shipping builds [Section 5]
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&dxgiFactory_)))) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "Failed to create DXGIFactory6.");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (!SelectHardwareAdapter(preferredAdapterName_, &adapter)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "No compatible D3D12 Feature Level 12_0 + SM 6.0 hardware adapter found.");
        return false;
    }

    if (!CreateDeviceAndCommandObjects(adapter.Get())) {
        return false;
    }

    if (!atlas_.Initialize()) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "Failed to initialize DirectWrite/WIC atlas.");
        return false;
    }

    if (!CreateCompositionSwapChainAndVisual(overlayHwnd_, widthPx_, heightPx_)) {
        return false;
    }

    if (!CreatePipelineStatesAndHeaps()) {
        return false;
    }

    if (!CreateAtlasTextures()) {
        return false;
    }

    if (!CreateRenderTargetViews()) {
        return false;
    }

    initialized_ = true;

    // Prewarm a valid transparent frame without showing a window [Section 5]
    if (!PrewarmTransparentFrame()) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Warning, "Graphics", "Initial transparent frame prewarm returned false.");
    }

    diagnostics_.RecordEvent(
        DiagnosticSeverity::Info,
        "Graphics",
        "Initialized D3D12 + DirectComposition renderer on adapter: " + WideToUtf8(selectedAdapterName_));
    return true;
}

void D3D12OverlayRenderer::Shutdown() {
    // Idempotent: explicit Shutdown() followed by destructor must be safe.
    if (shutdownCompleted_) {
        return;
    }
    shutdownCompleted_ = true;

    SetCompositorClockBoost(false);
    WaitForGpuIdle();

    for (auto& f : frames_) {
        if (f.uploadBuffer && f.mappedUploadPtr) {
            f.uploadBuffer->Unmap(0, nullptr);
            f.mappedUploadPtr = nullptr;
        }
        f.uploadBuffer.Reset();
        f.backBuffer.Reset();
        f.allocator.Reset();
        f.fenceValue = 0;
    }

    timestampReadbackBuffer_.Reset();
    timestampQueryHeap_.Reset();
    glyphUploadBuffer_.Reset();
    glyphTexture_.Reset();
    iconUploadBuffer_.Reset();
    iconTexture_.Reset();

    atlasPso_.Reset();
    atlasRootSignature_.Reset();
    ringPso_.Reset();
    ringRootSignature_.Reset();

    srvHeap_.Reset();
    rtvHeap_.Reset();

    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();

    frameLatencyWaitableHandle_.Reset();
    swapChain_.Reset();
    fence_.Reset();
    fenceEvent_.Reset();
    commandList_.Reset();
    commandQueue_.Reset();
    device_.Reset();
    dxgiFactory_.Reset();

    initialized_ = false;
}

bool D3D12OverlayRenderer::SelectHardwareAdapter(
    const std::wstring& preferredName,
    IDXGIAdapter1** outAdapter) {
    *outAdapter = nullptr;

    auto checkAdapterCompatible = [](IDXGIAdapter1* candidate) -> bool {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) return false;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return false;

        Microsoft::WRL::ComPtr<ID3D12Device> testDev;
        if (FAILED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDev)))) {
            return false;
        }

        // [R14] Query Shader Model 6.0 support
        D3D12_FEATURE_DATA_SHADER_MODEL smData{D3D_SHADER_MODEL_6_0};
        if (FAILED(testDev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smData, sizeof(smData))) ||
            smData.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
            return false;
        }
        return true;
    };

    // 1. Honor explicit user adapter override if specified (by description or LUID hex)
    if (!preferredName.empty() && preferredName != L"Auto") {
        const std::string preferredUtf8 = WideToUtf8(preferredName);
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; dxgiFactory_->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc))) {
                const std::string candidateLuid = AdapterLuidToHexString(desc.AdapterLuid);
                const bool matches = (preferredName == desc.Description) ||
                                     HexEqualsIgnoreCase(preferredUtf8, candidateLuid);
                if (matches && checkAdapterCompatible(candidate.Get())) {
                    selectedAdapterName_ = desc.Description;
                    selectedAdapterLuid_ = candidateLuid;
                    *outAdapter = candidate.Detach();
                    return true;
                }
            }
            candidate.Reset();
        }
    }

    // 2. Prefer the display-driving / power-efficient adapter (do not wake sleeping discrete GPU) [Section 6]
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0;
         dxgiFactory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && checkAdapterCompatible(candidate.Get())) {
            // Check if this adapter owns at least one active desktop output first
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(candidate->EnumOutputs(0, &output))) {
                selectedAdapterName_ = desc.Description;
                selectedAdapterLuid_ = AdapterLuidToHexString(desc.AdapterLuid);
                *outAdapter = candidate.Detach();
                return true;
            }
        }
        candidate.Reset();
    }

    // 3. Fallback to any compatible hardware adapter
    for (UINT i = 0;
         dxgiFactory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && checkAdapterCompatible(candidate.Get())) {
            selectedAdapterName_ = desc.Description;
            selectedAdapterLuid_ = AdapterLuidToHexString(desc.AdapterLuid);
            *outAdapter = candidate.Detach();
            return true;
        }
        candidate.Reset();
    }

    return false;
}

bool D3D12OverlayRenderer::CreateDeviceAndCommandObjects(IDXGIAdapter1* adapter) {
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "D3D12CreateDevice failed.");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_)))) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "CreateCommandQueue failed.");
        return false;
    }

    commandQueue_->GetTimestampFrequency(&gpuTimestampFrequency_);

    for (uint32_t i = 0; i < kBufferCount; ++i) {
        const HRESULT hrAllocator = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames_[i].allocator));
        if (FAILED(hrAllocator)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "CreateCommandAllocator failed (" + HrHex(hrAllocator) + ").");
            return false;
        }

        // Create persistently mapped upload buffer per frame context [Section 5]
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = kUploadBufferSizeBytes;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const HRESULT hrUpload = device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&frames_[i].uploadBuffer));
        if (FAILED(hrUpload)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Upload buffer CreateCommittedResource failed (" + HrHex(hrUpload) + ").");
            return false;
        }

        D3D12_RANGE readRange{0, 0};
        void* mapped = nullptr;
        const HRESULT hrMap = frames_[i].uploadBuffer->Map(0, &readRange, &mapped);
        if (FAILED(hrMap)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Upload buffer Map failed (" + HrHex(hrMap) + ").");
            return false;
        }
        frames_[i].mappedUploadPtr = static_cast<uint8_t*>(mapped);
    }

    const HRESULT hrCommandList = device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        frames_[0].allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList_));
    if (FAILED(hrCommandList)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "CreateCommandList failed (" + HrHex(hrCommandList) + ").");
        return false;
    }
    commandList_->Close();

    const HRESULT hrFence = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hrFence)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "CreateFence failed (" + HrHex(hrFence) + ").");
        return false;
    }
    fenceEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    nextFenceValue_ = 1;

    // Create GPU timestamp query heap & readback buffer
    D3D12_QUERY_HEAP_DESC qhDesc{};
    qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.Count = kBufferCount * 2;
    if (SUCCEEDED(device_->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&timestampQueryHeap_)))) {
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rbDesc{};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = sizeof(uint64_t) * kBufferCount * 2;
        rbDesc.Height = 1;
        rbDesc.DepthOrArraySize = 1;
        rbDesc.MipLevels = 1;
        rbDesc.Format = DXGI_FORMAT_UNKNOWN;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        device_->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&timestampReadbackBuffer_));
    }

    return true;
}

bool D3D12OverlayRenderer::CreateCompositionSwapChainAndVisual(
    HWND hwnd,
    uint32_t widthPx,
    uint32_t heightPx) {
    // [R1] Use IDXGIFactory2::CreateSwapChainForComposition with ID3D12CommandQueue, not ID3D12Device.
    // Two buffers, DXGI_FORMAT_B8G8R8A8_UNORM, sample count 1, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
    // DXGI_SCALING_STRETCH, and DXGI_ALPHA_MODE_PREMULTIPLIED.
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = widthPx;
    scDesc.Height = heightPx;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kBufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = dxgiFactory_->CreateSwapChainForComposition(
        commandQueue_.Get(), &scDesc, nullptr, &swapChain1);
    if (SUCCEEDED(hr) && swapChain1) {
        waitableSwapChain_ = true;
    } else {
        // Fallback if waitable flag is rejected on a composition swap chain
        scDesc.Flags = 0;
        waitableSwapChain_ = false;
        hr = dxgiFactory_->CreateSwapChainForComposition(
            commandQueue_.Get(), &scDesc, nullptr, &swapChain1);
        if (FAILED(hr) || !swapChain1) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "CreateSwapChainForComposition failed.");
            return false;
        }
    }

    const HRESULT hrAs = swapChain1.As(&swapChain_);
    if (FAILED(hrAs)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "IDXGISwapChain1.As(IDXGISwapChain3) failed (" + HrHex(hrAs) + ").");
        return false;
    }

    if (waitableSwapChain_) {
        // [R16] SetMaximumFrameLatency(1) as initial setting
        swapChain_->SetMaximumFrameLatency(1);
        frameLatencyWaitableHandle_.Reset(swapChain_->GetFrameLatencyWaitableObject());
    }

    // [R2] Create IDCompositionDesktopDevice through DCompositionCreateDevice2(nullptr, ...)
    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcompDevice_))) || !dcompDevice_) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "DCompositionCreateDevice2(nullptr, ...) failed.");
        return false;
    }

    if (hwnd) {
        if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics", "CreateTargetForHwnd failed.");
            return false;
        }

        const HRESULT hrVisual = dcompDevice_->CreateVisual(&dcompVisual_);
        if (FAILED(hrVisual)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "dcomp CreateVisual failed (" + HrHex(hrVisual) + ").");
            return false;
        }

        const HRESULT hrSetContent = dcompVisual_->SetContent(swapChain_.Get());
        if (FAILED(hrSetContent)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "dcomp SetContent failed (" + HrHex(hrSetContent) + ").");
            return false;
        }

        const HRESULT hrSetRoot = dcompTarget_->SetRoot(dcompVisual_.Get());
        if (FAILED(hrSetRoot)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "dcomp SetRoot failed (" + HrHex(hrSetRoot) + ").");
            return false;
        }

        const HRESULT hrCommit = dcompDevice_->Commit();
        if (FAILED(hrCommit)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "dcomp Commit failed (" + HrHex(hrCommit) + ").");
            return false;
        }
    }

    return true;
}

bool D3D12OverlayRenderer::CreatePipelineStatesAndHeaps() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kBufferCount;
    const HRESULT hrRtvHeap = device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_));
    if (FAILED(hrRtvHeap)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "RTV CreateDescriptorHeap failed (" + HrHex(hrRtvHeap) + ").");
        return false;
    }
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 2; // t0 = glyphAtlas, t1 = iconAtlas
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hrSrvHeap = device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap_));
    if (FAILED(hrSrvHeap)) {
        diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                 "SRV CreateDescriptorHeap failed (" + HrHex(hrSrvHeap) + ").");
        return false;
    }
    srvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 1. Ring SDF Root Signature (Root CBV at b0)
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.Descriptor.RegisterSpace = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
        const HRESULT hrSerialize = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
        if (FAILED(hrSerialize)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Ring D3D12SerializeRootSignature failed (" + HrHex(hrSerialize) + ").");
            return false;
        }
        const HRESULT hrRingRs = device_->CreateRootSignature(
            0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&ringRootSignature_));
        if (FAILED(hrRingRs)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Ring CreateRootSignature failed (" + HrHex(hrRingRs) + ").");
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = ringRootSignature_.Get();
        psoDesc.VS = {g_RingSdfVsBytecode, sizeof(g_RingSdfVsBytecode)};
        psoDesc.PS = {g_RingSdfPsBytecode, sizeof(g_RingSdfPsBytecode)};

        // Premultiplied alpha blending
        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.BlendState.RenderTarget[0] = blend;
        psoDesc.SampleMask = UINT_MAX;

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        const HRESULT hrRingPso = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&ringPso_));
        if (FAILED(hrRingPso)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Ring CreateGraphicsPipelineState failed (" + HrHex(hrRingPso) + ").");
            return false;
        }
    }

    // 2. Atlas Quad Root Signature (Param 0: CBV b0, Param 1: SRV table t0..t1, Static sampler s0)
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 2;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
        const HRESULT hrSerializeAtlas = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
        if (FAILED(hrSerializeAtlas)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Atlas D3D12SerializeRootSignature failed (" + HrHex(hrSerializeAtlas) + ").");
            return false;
        }
        const HRESULT hrAtlasRs = device_->CreateRootSignature(
            0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&atlasRootSignature_));
        if (FAILED(hrAtlasRs)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Atlas CreateRootSignature failed (" + HrHex(hrAtlasRs) + ").");
            return false;
        }

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadVertex, posPx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(QuadVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(QuadVertex, mode), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, offsetof(QuadVertex, flags), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = atlasRootSignature_.Get();
        psoDesc.VS = {g_AtlasQuadVsBytecode, sizeof(g_AtlasQuadVsBytecode)};
        psoDesc.PS = {g_AtlasQuadPsBytecode, sizeof(g_AtlasQuadPsBytecode)};
        psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};

        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.BlendState.RenderTarget[0] = blend;
        psoDesc.SampleMask = UINT_MAX;

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        const HRESULT hrAtlasPso = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&atlasPso_));
        if (FAILED(hrAtlasPso)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Atlas CreateGraphicsPipelineState failed (" + HrHex(hrAtlasPso) + ").");
            return false;
        }
    }

    return true;
}

bool D3D12OverlayRenderer::CreateAtlasTextures() {
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    // 1. Glyph Atlas (R8_UNORM, 1024x1024)
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = TextAndIconAtlas::kGlyphAtlasWidth;
        texDesc.Height = TextAndIconAtlas::kGlyphAtlasHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        const HRESULT hrGlyphTex = device_->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&glyphTexture_));
        if (FAILED(hrGlyphTex)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Glyph texture CreateCommittedResource failed (" + HrHex(hrGlyphTex) + ").");
            return false;
        }
        glyphTextureState_ = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_RESOURCE_DESC upDesc{};
        upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upDesc.Width = static_cast<uint64_t>(TextAndIconAtlas::kGlyphAtlasWidth) * TextAndIconAtlas::kGlyphAtlasHeight;
        upDesc.Height = 1;
        upDesc.DepthOrArraySize = 1;
        upDesc.MipLevels = 1;
        upDesc.Format = DXGI_FORMAT_UNKNOWN;
        upDesc.SampleDesc.Count = 1;
        upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const HRESULT hrGlyphUpload = device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&glyphUploadBuffer_));
        if (FAILED(hrGlyphUpload)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Glyph upload buffer CreateCommittedResource failed (" + HrHex(hrGlyphUpload) + ").");
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        device_->CreateShaderResourceView(
            glyphTexture_.Get(),
            &srvDesc,
            srvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    // 2. Icon Atlas (B8G8R8A8_UNORM, 512x512)
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = TextAndIconAtlas::kIconAtlasWidth;
        texDesc.Height = TextAndIconAtlas::kIconAtlasHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        const HRESULT hrIconTex = device_->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&iconTexture_));
        if (FAILED(hrIconTex)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Icon texture CreateCommittedResource failed (" + HrHex(hrIconTex) + ").");
            return false;
        }
        iconTextureState_ = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_RESOURCE_DESC upDesc{};
        upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upDesc.Width = static_cast<uint64_t>(TextAndIconAtlas::kIconAtlasWidth) * TextAndIconAtlas::kIconAtlasHeight * 4u;
        upDesc.Height = 1;
        upDesc.DepthOrArraySize = 1;
        upDesc.MipLevels = 1;
        upDesc.Format = DXGI_FORMAT_UNKNOWN;
        upDesc.SampleDesc.Count = 1;
        upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const HRESULT hrIconUpload = device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&iconUploadBuffer_));
        if (FAILED(hrIconUpload)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Icon upload buffer CreateCommittedResource failed (" + HrHex(hrIconUpload) + ").");
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += srvDescriptorSize_;
        device_->CreateShaderResourceView(iconTexture_.Get(), &srvDesc, handle);
    }

    return true;
}

bool D3D12OverlayRenderer::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        frames_[i].backBuffer.Reset();
        const HRESULT hrBuffer = swapChain_->GetBuffer(i, IID_PPV_ARGS(&frames_[i].backBuffer));
        if (FAILED(hrBuffer)) {
            diagnostics_.RecordEvent(DiagnosticSeverity::Error, "Graphics",
                                     "Swap chain GetBuffer(" + std::to_string(i) + ") failed (" + HrHex(hrBuffer) + ").");
            return false;
        }
        device_->CreateRenderTargetView(frames_[i].backBuffer.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    }
    return true;
}

bool D3D12OverlayRenderer::ResizeIfNeeded(uint32_t widthPx, uint32_t heightPx) {
    widthPx = std::clamp(widthPx, 128u, 2048u);
    heightPx = std::clamp(heightPx, 128u, 2048u);
    if (widthPx == widthPx_ && heightPx == heightPx_) {
        return true;
    }

    WaitForGpuIdle();
    for (auto& f : frames_) {
        f.backBuffer.Reset();
        f.fenceValue = 0;
    }

    const UINT flags = waitableSwapChain_ ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    if (FAILED(swapChain_->ResizeBuffers(kBufferCount, widthPx, heightPx, DXGI_FORMAT_B8G8R8A8_UNORM, flags))) {
        return TryRecoverFromDeviceLoss();
    }

    widthPx_ = widthPx;
    heightPx_ = heightPx;
    if (!CreateRenderTargetViews()) {
        return false;
    }

    if (dcompDevice_) {
        dcompDevice_->Commit();
    }
    return true;
}

void D3D12OverlayRenderer::PrecacheConfigAssets(const ConfigDocument& config, uint32_t dpi) {
    if (!initialized_) return;
    atlas_.PrecacheProfileIcons(config);
    for (const auto& act : config.actions) {
        atlas_.GetOrRasterizeText(Utf8ToWide(act.label), 14.0f, dpi, true);
    }
    for (const auto& ring : config.rings) {
        atlas_.GetOrRasterizeText(Utf8ToWide(ring.name), 13.0f, dpi, false);
    }
    atlas_.GetOrRasterizeText(L"Cancel", 12.0f, dpi, false);
    atlas_.GetOrRasterizeText(L"Right-click: Back", 11.0f, dpi, false);
    atlas_.GetOrRasterizeText(L"Target Unavailable", 11.0f, dpi, false);
}

void D3D12OverlayRenderer::UploadDirtyAtlases(ID3D12GraphicsCommandList* cmdList) {
    if (atlas_.IsGlyphAtlasDirty()) {
        if (glyphTextureState_ != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = glyphTexture_.Get();
            b.Transition.StateBefore = glyphTextureState_;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &b);
            glyphTextureState_ = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (SUCCEEDED(glyphUploadBuffer_->Map(0, &readRange, &mapped))) {
            std::memcpy(mapped, atlas_.GlyphAtlasPixels().data(), atlas_.GlyphAtlasPixels().size());
            glyphUploadBuffer_->Unmap(0, nullptr);
        }

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = glyphTexture_.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = glyphUploadBuffer_.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width = TextAndIconAtlas::kGlyphAtlasWidth;
        srcLoc.PlacedFootprint.Footprint.Height = TextAndIconAtlas::kGlyphAtlasHeight;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = TextAndIconAtlas::kGlyphAtlasWidth; // 1024 is multiple of 256

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = glyphTexture_.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
        glyphTextureState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        atlas_.ClearGlyphAtlasDirty();
    } else if (glyphTextureState_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = glyphTexture_.Get();
        b.Transition.StateBefore = glyphTextureState_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
        glyphTextureState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    if (atlas_.IsIconAtlasDirty()) {
        if (iconTextureState_ != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = iconTexture_.Get();
            b.Transition.StateBefore = iconTextureState_;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &b);
            iconTextureState_ = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (SUCCEEDED(iconUploadBuffer_->Map(0, &readRange, &mapped))) {
            std::memcpy(mapped, atlas_.IconAtlasPixels().data(), atlas_.IconAtlasPixels().size());
            iconUploadBuffer_->Unmap(0, nullptr);
        }

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = iconTexture_.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = iconUploadBuffer_.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width = TextAndIconAtlas::kIconAtlasWidth;
        srcLoc.PlacedFootprint.Footprint.Height = TextAndIconAtlas::kIconAtlasHeight;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = TextAndIconAtlas::kIconAtlasWidth * 4u; // 2048 is multiple of 256

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = iconTexture_.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
        iconTextureState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        atlas_.ClearIconAtlasDirty();
    } else if (iconTextureState_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = iconTexture_.Get();
        b.Transition.StateBefore = iconTextureState_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
        iconTextureState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

bool D3D12OverlayRenderer::PrewarmTransparentFrame() {
    OverlayRenderParams params{};
    params.layout.overlayBoundsPx = {0, 0, static_cast<int32_t>(widthPx_), static_cast<int32_t>(heightPx_)};
    params.layout.anchorInOverlayPx = {widthPx_ * 0.5f, heightPx_ * 0.5f};
    params.visual.masterOpacity = 0.0f;
    params.visual.openScale = 1.0f;
    const auto res = RenderFrame(params);
    WaitForGpuIdle();
    return res.rendered;
}

FrameTimingResult D3D12OverlayRenderer::RenderFrame(const OverlayRenderParams& params) {
    FrameTimingResult result{};
    if (!initialized_ || IsPermanentlyFailed()) {
        return result;
    }

    const int64_t cpuStartUs = clock_.NowMicroseconds();

    const UINT backBufferIdx = swapChain_->GetCurrentBackBufferIndex();
    FrameContext& ctx = frames_[backBufferIdx];

    // Wait on fence for this buffer context before resetting allocator or overwriting uploads [R6]
    if (ctx.fenceValue != 0 && fence_->GetCompletedValue() < ctx.fenceValue) {
        fence_->SetEventOnCompletion(ctx.fenceValue, fenceEvent_.Get());
        WaitForSingleObject(fenceEvent_.Get(), 100);
    }

    // Non-blocking GPU timestamp readback from the previous frame that used this buffer slot
    if (ctx.hasTimestampQuery && timestampReadbackBuffer_ && gpuTimestampFrequency_ > 0) {
        D3D12_RANGE readRange{
            sizeof(uint64_t) * backBufferIdx * 2,
            sizeof(uint64_t) * (backBufferIdx * 2 + 2)
        };
        uint64_t* mappedTimestamps = nullptr;
        if (SUCCEEDED(timestampReadbackBuffer_->Map(0, &readRange, reinterpret_cast<void**>(&mappedTimestamps)))) {
            const uint64_t t0 = mappedTimestamps[backBufferIdx * 2 + 0];
            const uint64_t t1 = mappedTimestamps[backBufferIdx * 2 + 1];
            D3D12_RANGE emptyRange{0, 0};
            timestampReadbackBuffer_->Unmap(0, &emptyRange);
            if (t1 >= t0) {
                result.previousFrameGpuWorkMs =
                    static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(gpuTimestampFrequency_);
            }
        }
    }

    if (FAILED(ctx.allocator->Reset()) ||
        FAILED(commandList_->Reset(ctx.allocator.Get(), ringPso_.Get()))) {
        result.deviceLost = !TryRecoverFromDeviceLoss();
        return result;
    }

    if (timestampQueryHeap_) {
        commandList_->EndQuery(timestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, backBufferIdx * 2 + 0);
    }

    // Build constant buffers and vertices if ring is visible
    std::vector<QuadVertex> quadVertices;
    const bool drawRingGeometry = (params.visual.masterOpacity > 0.002f && params.config != nullptr);

    if (drawRingGeometry) {
        // Pre-warm text glyphs so UploadDirtyAtlases uploads any newly shaped label before draw
        const RadialRing* activeRing = params.config->FindRing(params.activeRingId);

        const bool highContrast = params.systemHighContrast;
        const std::string& reducedTransOverride = params.config->preferences.reducedTransparencyOverride;
        const bool reducedTrans =
            (reducedTransOverride == "On") ||
            (reducedTransOverride != "Off" && params.systemReducedTransparency);
        const std::string& themePreference = params.config->preferences.theme;
        const bool lightTheme =
            (themePreference == "Light") ||
            (themePreference != "Dark" && params.systemLightMode);

        RingConstantsGpu rc{};
        rc.surfaceSizePx[0] = static_cast<float>(widthPx_);
        rc.surfaceSizePx[1] = static_cast<float>(heightPx_);
        rc.ringCenterPx[0] = params.layout.anchorInOverlayPx.x;
        rc.ringCenterPx[1] = params.layout.anchorInOverlayPx.y;
        rc.innerRadiusPx = params.layout.innerRadiusPx;
        rc.outerRadiusPx = params.layout.outerRadiusPx;
        rc.hubRadiusPx = params.layout.deadZoneRadiusPx;
        const float midR = std::max(1.0f, (params.layout.innerRadiusPx + params.layout.outerRadiusPx) * 0.5f);
        rc.sectorGapRad = params.layout.sectorGapPx / midR;
        rc.startAngleRad = params.layout.startAngleRad;
        rc.arcSpanRad = params.layout.arcSpanRad;
        rc.sectorCount = static_cast<uint32_t>(std::max(1u, params.layout.sectorCount));
        rc.selectedSector = params.selectedSector;
        rc.openScale = params.visual.openScale;
        rc.ringOpacity = params.visual.masterOpacity;
        rc.selectionAlpha = params.visual.highlightIntensity;
        rc.highlightAngleRad = params.visual.highlightAngleRad;
        rc.selectionLiftPx = (params.selectedSector >= 0 && params.selectedSector < 8)
            ? DipToPhysicalPx(params.visual.sectorLiftDip[static_cast<size_t>(params.selectedSector)], params.layout.dpi)
            : 0.0f;
        rc.confirmPulse = params.visual.confirmProgress;
        rc.dwellProgress = params.childRingDwellProgress;

        uint32_t flags = 0;
        if (highContrast) flags |= (1u << 0);
        if (reducedTrans) flags |= (1u << 1);
        if (lightTheme) flags |= (1u << 2);
        if (params.isChildRing) flags |= (1u << 3);
        rc.flags = flags;

        uint32_t availMask = 0;
        uint32_t childMask = 0;
        if (activeRing) {
            for (size_t i = 0; i < activeRing->sectorActionIds.size() && i < 8; ++i) {
                const bool avail = (i < params.sectorAvailable.size()) ? params.sectorAvailable[i] : true;
                if (avail) {
                    availMask |= (1u << i);
                }
                const ActionDefinition* actPtr = params.config->FindAction(activeRing->sectorActionIds[i]);
                if (actPtr && actPtr->type == ActionType::OpenChildRing) {
                    childMask |= (1u << i);
                }
            }
        }
        rc.sectorStateMask = availMask;
        rc.childRingSectorMask = childMask;

        const bool efficiencyQuality = (params.config->preferences.qualityPreset == "Efficiency");
        rc.shadowRadiusPx = efficiencyQuality
            ? DipToPhysicalPx(10.0f, params.layout.dpi)
            : DipToPhysicalPx(22.0f, params.layout.dpi);
        rc.rimIntensity = efficiencyQuality ? 0.65f : 1.0f;

        Color4F accent = ParseHexColor(params.config->preferences.accentColorHex, {0.47f, 0.78f, 1.0f, 1.0f});

        const float glassAlpha = std::clamp(params.config->preferences.glassOpacity, 0.45f, 1.0f);
        if (lightTheme) {
            WriteColor4(rc.glassTint, {0.93f, 0.95f, 0.98f, glassAlpha});
            WriteColor4(rc.glassHighlight, {0.99f, 0.995f, 1.0f, std::min(1.0f, glassAlpha + 0.06f)});
            WriteColor4(rc.accentColor, {accent.r * 0.80f, accent.g * 0.82f, accent.b * 0.95f, 1.0f});
            WriteColor4(rc.rimColor, {0.35f, 0.42f, 0.52f, 0.38f});
            WriteColor4(rc.shadowColor, {0.05f, 0.08f, 0.14f, 0.24f});
            WriteColor4(rc.disabledTint, {0.78f, 0.80f, 0.84f, glassAlpha * 0.55f});
        } else {
            // Midnight Glass [Section 8]: translucent charcoal, soft neutral-white typography, restrained ice-blue accent
            WriteColor4(rc.glassTint, {0.10f, 0.12f, 0.16f, glassAlpha});
            WriteColor4(rc.glassHighlight, {0.17f, 0.21f, 0.28f, std::min(1.0f, glassAlpha + 0.06f)});
            WriteColor4(rc.accentColor, accent);
            WriteColor4(rc.rimColor, {0.82f, 0.90f, 1.0f, 0.34f});
            WriteColor4(rc.shadowColor, {0.0f, 0.0f, 0.02f, 0.52f});
            WriteColor4(rc.disabledTint, {0.08f, 0.09f, 0.11f, glassAlpha * 0.48f});
        }

        std::memcpy(ctx.mappedUploadPtr + kRingConstantsOffset, &rc, sizeof(rc));

        AtlasConstantsGpu ac{};
        ac.surfaceSizePx[0] = static_cast<float>(widthPx_);
        ac.surfaceSizePx[1] = static_cast<float>(heightPx_);
        ac.ringCenterPx[0] = params.layout.anchorInOverlayPx.x;
        ac.ringCenterPx[1] = params.layout.anchorInOverlayPx.y;
        ac.openScale = params.visual.openScale;
        ac.ringOpacity = params.visual.masterOpacity;
        std::memcpy(ctx.mappedUploadPtr + kAtlasConstantsOffset, &ac, sizeof(ac));

        // Build icon & center label quads
        if (activeRing) {
            const float iconSizePx = DipToPhysicalPx(params.config->preferences.iconSizeDip, params.layout.dpi);
            for (size_t i = 0; i < params.layout.sectors.size() && i < activeRing->sectorActionIds.size(); ++i) {
                const ActionDefinition* actPtr = params.config->FindAction(activeRing->sectorActionIds[i]);
                if (!actPtr || actPtr->type == ActionType::None) {
                    continue;
                }
                const ActionDefinition& act = *actPtr;
                const SectorGeometry& sec = params.layout.sectors[i];
                const bool isSel = (static_cast<int>(i) == params.selectedSector);
                const bool isAvail = (i < params.sectorAvailable.size()) ? params.sectorAvailable[i] : true;

                float cx = sec.iconCenterPx.x;
                float cy = sec.iconCenterPx.y;
                if (isSel) {
                    const float liftPx = DipToPhysicalPx(
                        params.visual.sectorLiftDip[static_cast<size_t>(i)], params.layout.dpi);
                    cx += std::cos(sec.centerAngleRad) * liftPx;
                    cy += std::sin(sec.centerAngleRad) * liftPx;
                }

                const AtlasUvRect iconUv = atlas_.GetIconUv(act);
                Color4F iconColor = lightTheme
                    ? Color4F{0.12f, 0.15f, 0.20f, 0.95f}
                    : Color4F{0.94f, 0.96f, 0.99f, 0.96f};
                if (!isAvail) {
                    iconColor.a = 0.35f;
                } else if (isSel) {
                    iconColor = lightTheme
                        ? Color4F{0.05f, 0.10f, 0.18f, 1.0f}
                        : Color4F{1.0f, 1.0f, 1.0f, 1.0f};
                }

                TextAndIconAtlas::AppendQuad(
                    quadVertices,
                    cx - iconSizePx * 0.5f,
                    cy - iconSizePx * 0.5f,
                    iconSizePx,
                    iconSizePx,
                    iconUv,
                    iconColor,
                    1);
            }

            // Center / Selected-action label [Section 7: Keep text mostly in the center/selected-action label]
            std::wstring primaryText = Utf8ToWide(activeRing->name);
            std::wstring secondaryText = params.isChildRing ? L"Right-click: Back" : L"Cancel";
            bool selectedUnavailable = false;

            if (params.selectedSector >= 0 &&
                static_cast<size_t>(params.selectedSector) < activeRing->sectorActionIds.size()) {
                const auto& actId = activeRing->sectorActionIds[static_cast<size_t>(params.selectedSector)];
                const ActionDefinition* actPtr = params.config->FindAction(actId);
                if (actPtr) {
                    primaryText = Utf8ToWide(actPtr->label);
                    const bool avail = (static_cast<size_t>(params.selectedSector) < params.sectorAvailable.size())
                        ? params.sectorAvailable[static_cast<size_t>(params.selectedSector)]
                        : true;
                    if (!avail) {
                        secondaryText = !actPtr->approved ? L"Unapproved Action" : L"Target Unavailable";
                        selectedUnavailable = true;
                    } else if (actPtr->type == ActionType::OpenChildRing) {
                        secondaryText = L"Subring (Hold/Click)";
                    } else {
                        secondaryText.clear();
                    }
                }
            }

            const Color4F primaryTextColor = highContrast
                ? Color4F{1.0f, 1.0f, 1.0f, 1.0f}
                : (lightTheme ? Color4F{0.10f, 0.13f, 0.18f, 0.98f} : Color4F{0.95f, 0.97f, 1.0f, 0.98f});
            const Color4F secondaryTextColor = selectedUnavailable
                ? Color4F{1.0f, 0.55f, 0.45f, 0.92f}
                : (lightTheme ? Color4F{0.32f, 0.38f, 0.46f, 0.82f} : Color4F{0.68f, 0.75f, 0.85f, 0.82f});

            const Point2F labelPos = params.layout.centerLabelPosPx;
            const auto& primaryGlyph = atlas_.GetOrRasterizeText(primaryText, 13.5f, params.layout.dpi, true);
            if (primaryGlyph.widthPx > 0.0f) {
                const float yOffset = secondaryText.empty() ? 0.0f : -DipToPhysicalPx(7.0f, params.layout.dpi);
                TextAndIconAtlas::AppendQuad(
                    quadVertices,
                    std::round(labelPos.x - primaryGlyph.widthPx * 0.5f),
                    std::round(labelPos.y - primaryGlyph.heightPx * 0.5f + yOffset),
                    primaryGlyph.widthPx,
                    primaryGlyph.heightPx,
                    primaryGlyph.uv,
                    primaryTextColor,
                    0);
            }

            if (!secondaryText.empty()) {
                const auto& secGlyph = atlas_.GetOrRasterizeText(secondaryText, 10.5f, params.layout.dpi, false);
                if (secGlyph.widthPx > 0.0f) {
                    const float yOffset = DipToPhysicalPx(9.0f, params.layout.dpi);
                    TextAndIconAtlas::AppendQuad(
                        quadVertices,
                        std::round(labelPos.x - secGlyph.widthPx * 0.5f),
                        std::round(labelPos.y - secGlyph.heightPx * 0.5f + yOffset),
                        secGlyph.widthPx,
                        secGlyph.heightPx,
                        secGlyph.uv,
                        secondaryTextColor,
                        0);
                }
            }
        }

        const size_t maxVertices = kMaxQuadsPerFrame * 6;
        if (quadVertices.size() > maxVertices) {
            quadVertices.resize(maxVertices);
        }
        if (!quadVertices.empty()) {
            std::memcpy(
                ctx.mappedUploadPtr + kQuadVerticesOffset,
                quadVertices.data(),
                quadVertices.size() * sizeof(QuadVertex));
        }

        UploadDirtyAtlases(commandList_.Get());
    }

    // Transition back buffer PRESENT -> RENDER_TARGET [R6]
    D3D12_RESOURCE_BARRIER toRt{};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource = ctx.backBuffer.Get();
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(backBufferIdx) * rtvDescriptorSize_;

    // Clear transparent pixels to zero [Section 5]
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    if (drawRingGeometry) {
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(widthPx_), static_cast<float>(heightPx_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(widthPx_), static_cast<LONG>(heightPx_)};
        commandList_->RSSetViewports(1, &vp);
        commandList_->RSSetScissorRects(1, &scissor);
        commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // Pass 1: Procedural distance-field ring & glass material
        commandList_->SetPipelineState(ringPso_.Get());
        commandList_->SetGraphicsRootSignature(ringRootSignature_.Get());
        commandList_->SetGraphicsRootConstantBufferView(
            0, ctx.uploadBuffer->GetGPUVirtualAddress() + kRingConstantsOffset);
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList_->DrawInstanced(3, 1, 0, 0);

        // Pass 2: Icons & grayscale text labels from atlases
        if (!quadVertices.empty()) {
            commandList_->SetPipelineState(atlasPso_.Get());
            commandList_->SetGraphicsRootSignature(atlasRootSignature_.Get());
            commandList_->SetGraphicsRootConstantBufferView(
                0, ctx.uploadBuffer->GetGPUVirtualAddress() + kAtlasConstantsOffset);

            ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
            commandList_->SetDescriptorHeaps(1, heaps);
            commandList_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = ctx.uploadBuffer->GetGPUVirtualAddress() + kQuadVerticesOffset;
            vbv.SizeInBytes = static_cast<UINT>(quadVertices.size() * sizeof(QuadVertex));
            vbv.StrideInBytes = sizeof(QuadVertex);
            commandList_->IASetVertexBuffers(0, 1, &vbv);
            commandList_->DrawInstanced(static_cast<UINT>(quadVertices.size()), 1, 0, 0);
        }
    }

    // Transition back buffer RENDER_TARGET -> PRESENT [R6]
    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = ctx.backBuffer.Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &toPresent);

    if (timestampQueryHeap_ && timestampReadbackBuffer_) {
        commandList_->EndQuery(timestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, backBufferIdx * 2 + 1);
        commandList_->ResolveQueryData(
            timestampQueryHeap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            backBufferIdx * 2,
            2,
            timestampReadbackBuffer_.Get(),
            sizeof(uint64_t) * backBufferIdx * 2);
        ctx.hasTimestampQuery = true;
    }

    commandList_->Close();

    ID3D12CommandList* lists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, lists);

    HRESULT presentHr = swapChain_->Present(1, 0);
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
        result.deviceLost = !TryRecoverFromDeviceLoss();
        return result;
    }

    const uint64_t signalVal = nextFenceValue_++;
    commandQueue_->Signal(fence_.Get(), signalVal);
    ctx.fenceValue = signalVal;

    const int64_t cpuEndUs = clock_.NowMicroseconds();
    result.rendered = true;
    result.cpuWorkMs = static_cast<double>(cpuEndUs - cpuStartUs) / 1000.0;
    result.frameSubmittedQpcUs = cpuEndUs;
    return result;
}

bool D3D12OverlayRenderer::CaptureCurrentBackBufferToPng(const std::wstring& outputPath) {
    if (!initialized_) {
        return false;
    }
    WaitForGpuIdle();

    // Since SwapEffect is FLIP_SEQUENTIAL, GetCurrentBackBufferIndex points to the next buffer to render;
    // the just-presented buffer is (GetCurrentBackBufferIndex() + kBufferCount - 1) % kBufferCount.
    const UINT presentedIdx = (swapChain_->GetCurrentBackBufferIndex() + kBufferCount - 1) % kBufferCount;
    ID3D12Resource* srcTex = frames_[presentedIdx].backBuffer.Get();
    if (!srcTex) return false;

    const uint32_t rowPitch = (widthPx_ * 4u + 255u) & ~255u;
    const uint64_t readbackSize = static_cast<uint64_t>(rowPitch) * heightPx_;

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = readbackSize;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> rbBuffer;
    if (FAILED(device_->CreateCommittedResource(
            &rbHeap,
            D3D12_HEAP_FLAG_NONE,
            &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&rbBuffer)))) {
        return false;
    }

    frames_[0].allocator->Reset();
    commandList_->Reset(frames_[0].allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = srcTex;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuffer.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = widthPx_;
    dstLoc.PlacedFootprint.Footprint.Height = heightPx_;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = srcTex;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    commandList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toCopy;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &toPresent);

    commandList_->Close();
    ID3D12CommandList* lists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, lists);
    WaitForGpuIdle();

    uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(readbackSize)};
    if (FAILED(rbBuffer->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        rbBuffer->Unmap(0, nullptr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE))) {
        rbBuffer->Unmap(0, nullptr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        rbBuffer->Unmap(0, nullptr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frameEncode;
    if (FAILED(encoder->CreateNewFrame(&frameEncode, nullptr)) ||
        FAILED(frameEncode->Initialize(nullptr)) ||
        FAILED(frameEncode->SetSize(widthPx_, heightPx_))) {
        rbBuffer->Unmap(0, nullptr);
        return false;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    frameEncode->SetPixelFormat(&pixelFormat);
    const HRESULT writeHr = frameEncode->WritePixels(heightPx_, rowPitch, static_cast<UINT>(readbackSize), mapped);
    rbBuffer->Unmap(0, nullptr);

    if (FAILED(writeHr) || FAILED(frameEncode->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }

    return true;
}

void D3D12OverlayRenderer::SetCompositorClockBoost(bool enable) {
    if (compositorBoostActive_ == enable) {
        return;
    }
    if (pfnBoostCompositorClock_) {
        // [R7] Pair any compositor boost request with release
        pfnBoostCompositorClock_(enable ? TRUE : FALSE);
    }
    compositorBoostActive_ = enable;
}

DWORD D3D12OverlayRenderer::WaitForNextFrameOrWake(HANDLE wakeEvent, DWORD timeoutMs) {
    // [R7] Integrate compositor-clock scheduling where appropriate, with an input/quit wake path
    // and an explicitly documented fallback. Do not serially wait for both a fresh compositor tick
    // and another full presentation interval on every frame.
    if (pfnWaitForCompositorClock_) {
        const HANDLE handles[1] = {wakeEvent};
        const UINT count = wakeEvent ? 1u : 0u;
        return pfnWaitForCompositorClock_(count, count ? handles : nullptr, timeoutMs);
    }

    if (waitableSwapChain_ && frameLatencyWaitableHandle_.IsValid()) {
        if (wakeEvent) {
            const HANDLE handles[2] = {wakeEvent, frameLatencyWaitableHandle_.Get()};
            return WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
        }
        return WaitForSingleObject(frameLatencyWaitableHandle_.Get(), timeoutMs);
    }

    if (wakeEvent) {
        return WaitForSingleObject(wakeEvent, timeoutMs);
    }
    return WAIT_TIMEOUT;
}

void D3D12OverlayRenderer::WaitForGpuIdle() {
    if (!commandQueue_ || !fence_ || !fenceEvent_.IsValid()) {
        return;
    }
    const uint64_t val = nextFenceValue_++;
    if (SUCCEEDED(commandQueue_->Signal(fence_.Get(), val))) {
        if (fence_->GetCompletedValue() < val) {
            fence_->SetEventOnCompletion(val, fenceEvent_.Get());
            WaitForSingleObject(fenceEvent_.Get(), 2000);
        }
    }
}

bool D3D12OverlayRenderer::TryRecoverFromDeviceLoss() {
    ++recoveryAttempts_;
    diagnostics_.RecordEvent(
        DiagnosticSeverity::Warning,
        "Graphics",
        "D3D12 device loss detected; attempting recovery #" + std::to_string(recoveryAttempts_));

    if (recoveryAttempts_ >= 3) {
        diagnostics_.RecordEvent(
            DiagnosticSeverity::Error,
            "Graphics",
            "Repeated D3D12 device recovery failures; halting rendering to prevent crash-loop.");
        Shutdown();
        return false;
    }

    const HWND hwnd = overlayHwnd_;
    const uint32_t w = widthPx_;
    const uint32_t h = heightPx_;
    const std::wstring pref = preferredAdapterName_;
    Shutdown();
    return Initialize(hwnd, w, h, pref);
}

} // namespace keychain
