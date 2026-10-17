#include "diagnostics/diagnostics_manager.h"
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <shellscalingapi.h>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <chrono>

namespace keychain {

using json = nlohmann::json;

DiagnosticsManager::DiagnosticsManager(std::shared_ptr<IClock> clock)
    : clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = std::make_shared<SystemQpcClock>();
    }
}

void DiagnosticsManager::SetRedactUserPaths(bool redact) noexcept {
    AcquireSRWLockExclusive(&lock_);
    redactUserPaths_ = redact;
    ReleaseSRWLockExclusive(&lock_);
}

bool DiagnosticsManager::GetRedactUserPaths() const noexcept {
    AcquireSRWLockShared(&lock_);
    bool r = redactUserPaths_;
    ReleaseSRWLockShared(&lock_);
    return r;
}

std::string DiagnosticsManager::RedactPathString(std::string_view input, bool redactEnabled) {
    std::string out(input);
    if (!redactEnabled || out.empty()) {
        return out;
    }

    wchar_t userProfileBuf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", userProfileBuf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string userProfileUtf8 = WideToUtf8(std::wstring_view(userProfileBuf, len));
        if (!userProfileUtf8.empty()) {
            // Case-insensitive replacement of C:\Users\<name> with %USERPROFILE%
            std::string lowerOut = out;
            std::string lowerProf = userProfileUtf8;
            std::transform(lowerOut.begin(), lowerOut.end(), lowerOut.begin(), ::tolower);
            std::transform(lowerProf.begin(), lowerProf.end(), lowerProf.begin(), ::tolower);

            size_t pos = 0;
            while ((pos = lowerOut.find(lowerProf, pos)) != std::string::npos) {
                out.replace(pos, userProfileUtf8.size(), "%USERPROFILE%");
                lowerOut.replace(pos, lowerProf.size(), "%userprofile%");
                pos += 13;
            }
        }
    }
    return out;
}

PercentileStats DiagnosticsManager::ComputePercentiles(std::vector<double> values) {
    PercentileStats stats{};
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.sampleCount = values.size();
    stats.minVal      = values.front();
    stats.maxVal      = values.back();

    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    stats.mean = sum / static_cast<double>(values.size());

    auto atPct = [&](double pct) -> double {
        if (values.size() == 1) return values.front();
        double idx = pct * static_cast<double>(values.size() - 1);
        size_t lo  = static_cast<size_t>(std::floor(idx));
        size_t hi  = static_cast<size_t>(std::ceil(idx));
        double frac = idx - static_cast<double>(lo);
        return values[lo] * (1.0 - frac) + values[hi] * frac;
    };

    stats.p50 = atPct(0.50);
    stats.p95 = atPct(0.95);
    stats.p99 = atPct(0.99);
    return stats;
}

void DiagnosticsManager::RecordEvent(DiagnosticSeverity severity, std::string_view category, std::string_view message) {
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    AcquireSRWLockExclusive(&lock_);
    DiagnosticEntry entry;
    entry.timestampUnixMs = nowMs;
    entry.severity        = severity;
    entry.category        = std::string(category);
    entry.message         = RedactPathString(message, redactUserPaths_);

    if (entries_.size() >= kMaxHistoryEntries) {
        entries_.pop_front();
    }
    entries_.push_back(std::move(entry));
    ReleaseSRWLockExclusive(&lock_);
}

void DiagnosticsManager::RecordActivationLatency(
    uint64_t generationId,
    int64_t receiptQpc,
    int64_t recognizedQpc,
    int64_t renderWakeQpc,
    int64_t firstSubmitQpc
) {
    ActivationLatencySample sample;
    sample.generationId     = generationId;
    sample.receiptQpc       = receiptQpc;
    sample.recognizedQpc    = recognizedQpc;
    sample.renderWakeQpc    = renderWakeQpc;
    sample.firstSubmitQpc   = firstSubmitQpc;
    sample.receiptToWakeMs  = std::max(0.0, clock_->TicksToMilliseconds(renderWakeQpc - receiptQpc));
    sample.wakeToSubmitMs   = std::max(0.0, clock_->TicksToMilliseconds(firstSubmitQpc - renderWakeQpc));
    sample.totalHotkeyToSubmitMs = std::max(0.0, clock_->TicksToMilliseconds(firstSubmitQpc - receiptQpc));

    AcquireSRWLockExclusive(&lock_);
    ++totalActivations_;
    if (activationSamples_.size() >= kMaxTimingSamples) {
        activationSamples_.pop_front();
    }
    activationSamples_.push_back(sample);
    ReleaseSRWLockExclusive(&lock_);
}

void DiagnosticsManager::RecordFrameTiming(
    uint64_t frameIndex,
    int64_t submitQpc,
    double cpuWorkMs,
    double gpuWorkMs,
    double frameIntervalMs
) {
    FrameTimingSample sample;
    sample.frameIndex      = frameIndex;
    sample.submitQpc       = submitQpc;
    sample.cpuWorkMs       = std::max(0.0, cpuWorkMs);
    sample.gpuWorkMs       = std::max(0.0, gpuWorkMs);
    sample.frameIntervalMs = std::max(0.0, frameIntervalMs);

    AcquireSRWLockExclusive(&lock_);
    ++totalSubmittedFrames_;
    if (frameSamples_.size() >= kMaxTimingSamples) {
        frameSamples_.pop_front();
    }
    frameSamples_.push_back(sample);
    ReleaseSRWLockExclusive(&lock_);
}

void DiagnosticsManager::SetGraphicsHardwareInfo(
    std::string selectedAdapterName,
    std::string selectedAdapterLuid,
    std::string featureLevel,
    std::string shaderModel,
    std::string compositorClockStatus,
    std::vector<AdapterDiagnosticInfo> allAdapters
) {
    AcquireSRWLockExclusive(&lock_);
    selectedAdapterName_   = std::move(selectedAdapterName);
    selectedAdapterLuid_   = std::move(selectedAdapterLuid);
    featureLevel_          = std::move(featureLevel);
    shaderModel_           = std::move(shaderModel);
    compositorClockStatus_ = std::move(compositorClockStatus);
    adapters_              = std::move(allAdapters);
    ReleaseSRWLockExclusive(&lock_);
}

void DiagnosticsManager::SetInputStatus(
    bool kbInstalled,
    bool mouseInstalled,
    bool ctrlAltSpaceAvail,
    uint32_t reinstallCount
) {
    AcquireSRWLockExclusive(&lock_);
    kbInstalled_        = kbInstalled;
    mouseInstalled_     = mouseInstalled;
    ctrlAltSpaceAvail_  = ctrlAltSpaceAvail;
    hookReinstallCount_ = reinstallCount;
    ReleaseSRWLockExclusive(&lock_);
}

std::vector<MonitorDiagnosticInfo> DiagnosticsManager::EnumerateSystemMonitors() {
    std::vector<MonitorDiagnosticInfo> list;
    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* outList = reinterpret_cast<std::vector<MonitorDiagnosticInfo>*>(lParam);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                MonitorDiagnosticInfo info;
                info.deviceName = WideToUtf8(mi.szDevice);
                info.boundsPx   = RectI{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
                info.workAreaPx = RectI{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};
                info.isPrimary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

                UINT dpiX = 96, dpiY = 96;
                if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                    info.dpi = dpiX;
                }

                DEVMODEW dm{};
                dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) {
                    if (dm.dmDisplayFrequency > 1) {
                        info.refreshRateHz = dm.dmDisplayFrequency;
                    }
                }
                outList->push_back(std::move(info));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list)
    );
    return list;
}

DiagnosticsSnapshot DiagnosticsManager::CaptureSnapshot() const {
    DiagnosticsSnapshot snap;
    snap.monitors = EnumerateSystemMonitors();
    for (const auto& m : snap.monitors) {
        if (m.isPrimary) {
            snap.activeMonitorRefreshHz = m.refreshRateHz;
            snap.activeMonitorDpi       = m.dpi;
            break;
        }
    }
    if (!snap.monitors.empty() && snap.activeMonitorRefreshHz == 60) {
        snap.activeMonitorRefreshHz = snap.monitors.front().refreshRateHz;
        snap.activeMonitorDpi       = snap.monitors.front().dpi;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        snap.privateBytesMib = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        snap.workingSetMib   = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }

    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        snap.handleCount = handles;
    }

    std::vector<double> actLatencies;
    std::vector<double> cpuTimes;
    std::vector<double> gpuTimes;
    std::vector<double> frameIntervals;

    AcquireSRWLockShared(&lock_);
    snap.selectedAdapterName   = selectedAdapterName_;
    snap.selectedAdapterLuid   = selectedAdapterLuid_;
    snap.featureLevel          = featureLevel_;
    snap.shaderModel           = shaderModel_;
    snap.compositorClockStatus = compositorClockStatus_;
    snap.adapters              = adapters_;
    snap.keyboardHookInstalled = kbInstalled_;
    snap.mouseHookInstalled    = mouseInstalled_;
    snap.ctrlAltSpaceAvailable = ctrlAltSpaceAvail_;
    snap.hookReinstallCount    = hookReinstallCount_;
    snap.totalSubmittedFrames  = totalSubmittedFrames_;
    snap.totalActivations      = totalActivations_;

    actLatencies.reserve(activationSamples_.size());
    for (const auto& s : activationSamples_) {
        actLatencies.push_back(s.totalHotkeyToSubmitMs);
    }

    cpuTimes.reserve(frameSamples_.size());
    gpuTimes.reserve(frameSamples_.size());
    frameIntervals.reserve(frameSamples_.size());
    for (const auto& f : frameSamples_) {
        cpuTimes.push_back(f.cpuWorkMs);
        if (f.gpuWorkMs > 0.0) {
            gpuTimes.push_back(f.gpuWorkMs);
        }
        if (f.frameIntervalMs > 0.0 && f.frameIntervalMs < 250.0) {
            frameIntervals.push_back(f.frameIntervalMs);
        }
    }

    snap.recentEntries.assign(entries_.begin(), entries_.end());
    ReleaseSRWLockShared(&lock_);

    snap.hotkeyToFirstSubmitMs = ComputePercentiles(std::move(actLatencies));
    snap.cpuFrameWorkMs        = ComputePercentiles(std::move(cpuTimes));
    snap.gpuFrameWorkMs        = ComputePercentiles(std::move(gpuTimes));
    snap.activeFrameIntervalMs = ComputePercentiles(std::move(frameIntervals));
    return snap;
}

static json PercentileStatsToJson(const PercentileStats& s) {
    return json{
        {"sampleCount", s.sampleCount},
        {"min", s.minVal},
        {"p50", s.p50},
        {"p95", s.p95},
        {"p99", s.p99},
        {"max", s.maxVal},
        {"mean", s.mean}
    };
}

std::string DiagnosticsManager::SerializeSnapshotToJson(const DiagnosticsSnapshot& snap, bool pretty) const {
    json j;
    j["selectedAdapterName"]   = snap.selectedAdapterName;
    j["selectedAdapterLuid"]   = snap.selectedAdapterLuid;
    j["featureLevel"]          = snap.featureLevel;
    j["shaderModel"]           = snap.shaderModel;
    j["compositorClockStatus"] = snap.compositorClockStatus;
    j["activeMonitorRefreshHz"] = snap.activeMonitorRefreshHz;
    j["activeMonitorDpi"]      = snap.activeMonitorDpi;
    j["privateBytesMib"]       = snap.privateBytesMib;
    j["workingSetMib"]         = snap.workingSetMib;
    j["handleCount"]           = snap.handleCount;
    j["totalSubmittedFrames"]  = snap.totalSubmittedFrames;
    j["totalActivations"]      = snap.totalActivations;
    j["hookReinstallCount"]    = snap.hookReinstallCount;
    j["keyboardHookInstalled"] = snap.keyboardHookInstalled;
    j["mouseHookInstalled"]    = snap.mouseHookInstalled;
    j["ctrlAltSpaceAvailable"] = snap.ctrlAltSpaceAvailable;

    j["metrics"] = {
        {"hotkeyToFirstSubmitMs", PercentileStatsToJson(snap.hotkeyToFirstSubmitMs)},
        {"cpuFrameWorkMs", PercentileStatsToJson(snap.cpuFrameWorkMs)},
        {"gpuFrameWorkMs", PercentileStatsToJson(snap.gpuFrameWorkMs)},
        {"activeFrameIntervalMs", PercentileStatsToJson(snap.activeFrameIntervalMs)}
    };

    json monArr = json::array();
    for (const auto& m : snap.monitors) {
        monArr.push_back({
            {"deviceName", m.deviceName},
            {"widthPx", m.boundsPx.Width()},
            {"heightPx", m.boundsPx.Height()},
            {"refreshRateHz", m.refreshRateHz},
            {"dpi", m.dpi},
            {"isPrimary", m.isPrimary}
        });
    }
    j["monitors"] = std::move(monArr);

    json adpArr = json::array();
    for (const auto& a : snap.adapters) {
        adpArr.push_back({
            {"description", a.description},
            {"luidHex", a.luidHex},
            {"dedicatedVideoMemoryMb", a.dedicatedVideoMemoryMb},
            {"isDiscrete", a.isDiscrete},
            {"isSelected", a.isSelected}
        });
    }
    j["adapters"] = std::move(adpArr);

    json evArr = json::array();
    for (const auto& e : snap.recentEntries) {
        const char* sevStr = (e.severity == DiagnosticSeverity::Error) ? "Error" :
                             (e.severity == DiagnosticSeverity::Warning) ? "Warning" : "Info";
        evArr.push_back({
            {"timestampUnixMs", e.timestampUnixMs},
            {"severity", sevStr},
            {"category", e.category},
            {"message", e.message}
        });
    }
    j["recentEntries"] = std::move(evArr);

    return pretty ? j.dump(2) : j.dump();
}

bool DiagnosticsManager::ExportDiagnosticsReport(const std::filesystem::path& outputPath, bool includeRawSamples) const {
    DiagnosticsSnapshot snap = CaptureSnapshot();
    json j = json::parse(SerializeSnapshotToJson(snap, false));

    if (includeRawSamples) {
        AcquireSRWLockShared(&lock_);
        json rawAct = json::array();
        for (const auto& s : activationSamples_) {
            rawAct.push_back({
                {"generationId", s.generationId},
                {"receiptQpc", s.receiptQpc},
                {"recognizedQpc", s.recognizedQpc},
                {"renderWakeQpc", s.renderWakeQpc},
                {"firstSubmitQpc", s.firstSubmitQpc},
                {"receiptToWakeMs", s.receiptToWakeMs},
                {"wakeToSubmitMs", s.wakeToSubmitMs},
                {"totalHotkeyToSubmitMs", s.totalHotkeyToSubmitMs}
            });
        }
        j["rawActivationSamples"] = std::move(rawAct);

        json rawFrames = json::array();
        for (const auto& f : frameSamples_) {
            rawFrames.push_back({
                {"frameIndex", f.frameIndex},
                {"submitQpc", f.submitQpc},
                {"cpuWorkMs", f.cpuWorkMs},
                {"gpuWorkMs", f.gpuWorkMs},
                {"frameIntervalMs", f.frameIntervalMs}
            });
        }
        j["rawFrameSamples"] = std::move(rawFrames);
        ReleaseSRWLockShared(&lock_);
    }

    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }
    std::ofstream ofs(outputPath, std::ios::binary);
    if (!ofs) return false;
    std::string outText = j.dump(2);
    ofs.write(outText.data(), static_cast<std::streamsize>(outText.size()));
    return static_cast<bool>(ofs);
}

} // namespace keychain
