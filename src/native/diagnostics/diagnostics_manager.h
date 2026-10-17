#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <deque>
#include <filesystem>

namespace keychain {

struct PercentileStats {
    size_t sampleCount = 0;
    double minVal      = 0.0;
    double p50         = 0.0;
    double p95         = 0.0;
    double p99         = 0.0;
    double maxVal      = 0.0;
    double mean        = 0.0;
};

struct ActivationLatencySample {
    uint64_t generationId     = 0;
    int64_t  receiptQpc       = 0;
    int64_t  recognizedQpc    = 0;
    int64_t  renderWakeQpc    = 0;
    int64_t  firstSubmitQpc   = 0;
    double   receiptToWakeMs  = 0.0;
    double   wakeToSubmitMs   = 0.0;
    double   totalHotkeyToSubmitMs = 0.0;
};

struct FrameTimingSample {
    uint64_t frameIndex      = 0;
    int64_t  submitQpc       = 0;
    double   cpuWorkMs       = 0.0;
    double   gpuWorkMs       = 0.0;
    double   frameIntervalMs = 0.0;
};

enum class DiagnosticSeverity {
    Info,
    Warning,
    Error
};

struct DiagnosticEntry {
    int64_t            timestampUnixMs = 0;
    DiagnosticSeverity severity        = DiagnosticSeverity::Info;
    std::string        category;
    std::string        message;
};

struct MonitorDiagnosticInfo {
    std::string deviceName;
    RectI       boundsPx{};
    RectI       workAreaPx{};
    uint32_t    refreshRateHz = 60;
    uint32_t    dpi           = 96;
    bool        isPrimary     = false;
};

struct AdapterDiagnosticInfo {
    std::string description;
    std::string luidHex;
    uint64_t    dedicatedVideoMemoryMb = 0;
    bool        isDiscrete             = false;
    bool        isSelected             = false;
};

struct DiagnosticsSnapshot {
    std::string                        selectedAdapterName;
    std::string                        selectedAdapterLuid;
    std::string                        featureLevel;
    std::string                        shaderModel;
    std::string                        compositorClockStatus;
    uint32_t                           activeMonitorRefreshHz = 60;
    uint32_t                           activeMonitorDpi       = 96;
    double                             privateBytesMib        = 0.0;
    double                             workingSetMib          = 0.0;
    uint32_t                           handleCount            = 0;
    uint64_t                           totalSubmittedFrames   = 0;
    uint64_t                           totalActivations       = 0;
    uint32_t                           hookReinstallCount     = 0;
    bool                               keyboardHookInstalled  = false;
    bool                               mouseHookInstalled     = false;
    bool                               ctrlAltSpaceAvailable  = true;

    PercentileStats                    hotkeyToFirstSubmitMs;
    PercentileStats                    cpuFrameWorkMs;
    PercentileStats                    gpuFrameWorkMs;
    PercentileStats                    activeFrameIntervalMs;

    std::vector<MonitorDiagnosticInfo> monitors;
    std::vector<AdapterDiagnosticInfo> adapters;
    std::vector<DiagnosticEntry>       recentEntries;
};

class DiagnosticsManager {
public:
    static constexpr size_t kMaxHistoryEntries = 100;
    static constexpr size_t kMaxTimingSamples  = 4096;

    explicit DiagnosticsManager(std::shared_ptr<IClock> clock = std::make_shared<SystemQpcClock>());

    void SetRedactUserPaths(bool redact) noexcept;
    bool GetRedactUserPaths() const noexcept;

    void RecordEvent(DiagnosticSeverity severity, std::string_view category, std::string_view message);
    void RecordActivationLatency(uint64_t generationId, int64_t receiptQpc, int64_t recognizedQpc, int64_t renderWakeQpc, int64_t firstSubmitQpc);
    void RecordFrameTiming(uint64_t frameIndex, int64_t submitQpc, double cpuWorkMs, double gpuWorkMs, double frameIntervalMs);

    void SetGraphicsHardwareInfo(
        std::string selectedAdapterName,
        std::string selectedAdapterLuid,
        std::string featureLevel,
        std::string shaderModel,
        std::string compositorClockStatus,
        std::vector<AdapterDiagnosticInfo> allAdapters
    );

    void SetInputStatus(bool kbInstalled, bool mouseInstalled, bool ctrlAltSpaceAvail, uint32_t reinstallCount);

    DiagnosticsSnapshot CaptureSnapshot() const;
    std::string SerializeSnapshotToJson(const DiagnosticsSnapshot& snap, bool pretty = true) const;
    bool ExportDiagnosticsReport(const std::filesystem::path& outputPath, bool includeRawSamples = true) const;

    static PercentileStats ComputePercentiles(std::vector<double> values);
    static std::string RedactPathString(std::string_view input, bool redactEnabled = true);
    static std::vector<MonitorDiagnosticInfo> EnumerateSystemMonitors();

private:
    std::shared_ptr<IClock> clock_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    bool redactUserPaths_ = true;

    std::string selectedAdapterName_   = "Initializing";
    std::string selectedAdapterLuid_   = "";
    std::string featureLevel_          = "D3D_FEATURE_LEVEL_12_0";
    std::string shaderModel_           = "SM_6_0";
    std::string compositorClockStatus_ = "WaitableSwapChain";
    std::vector<AdapterDiagnosticInfo> adapters_;

    bool     kbInstalled_        = false;
    bool     mouseInstalled_     = false;
    bool     ctrlAltSpaceAvail_  = true;
    uint32_t hookReinstallCount_ = 0;

    uint64_t totalSubmittedFrames_ = 0;
    uint64_t totalActivations_     = 0;

    std::deque<ActivationLatencySample> activationSamples_;
    std::deque<FrameTimingSample>       frameSamples_;
    std::deque<DiagnosticEntry>         entries_;
};

} // namespace keychain
