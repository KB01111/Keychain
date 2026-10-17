using System.Text.Json.Serialization;

namespace Keychain.Settings.Models;

public sealed class PercentileStats
{
    [JsonPropertyName("sampleCount")] public ulong SampleCount { get; set; }
    [JsonPropertyName("min")] public double Min { get; set; }
    [JsonPropertyName("p50")] public double P50 { get; set; }
    [JsonPropertyName("p95")] public double P95 { get; set; }
    [JsonPropertyName("p99")] public double P99 { get; set; }
    [JsonPropertyName("max")] public double Max { get; set; }
    [JsonPropertyName("mean")] public double Mean { get; set; }

    public string Describe() => SampleCount == 0 ? "no samples" : $"p50 {P50:F2} / p95 {P95:F2} / p99 {P99:F2} ms ({SampleCount} samples)";
}

public sealed class MetricsSummary
{
    [JsonPropertyName("hotkeyToFirstSubmitMs")] public PercentileStats HotkeyToFirstSubmitMs { get; set; } = new();
    [JsonPropertyName("cpuFrameWorkMs")] public PercentileStats CpuFrameWorkMs { get; set; } = new();
    [JsonPropertyName("gpuFrameWorkMs")] public PercentileStats GpuFrameWorkMs { get; set; } = new();
    [JsonPropertyName("activeFrameIntervalMs")] public PercentileStats ActiveFrameIntervalMs { get; set; } = new();
}

public sealed class MonitorInfo
{
    [JsonPropertyName("deviceName")] public string DeviceName { get; set; } = "";
    [JsonPropertyName("widthPx")] public int WidthPx { get; set; }
    [JsonPropertyName("heightPx")] public int HeightPx { get; set; }
    [JsonPropertyName("refreshRateHz")] public int RefreshRateHz { get; set; }
    [JsonPropertyName("dpi")] public int Dpi { get; set; }
    [JsonPropertyName("isPrimary")] public bool IsPrimary { get; set; }

    public string Describe() => $"{WidthPx}x{HeightPx} @ {RefreshRateHz} Hz, {Dpi} DPI{(IsPrimary ? " (primary)" : "")}";
}

public sealed class AdapterInfo
{
    [JsonPropertyName("description")] public string Description { get; set; } = "";
    [JsonPropertyName("luidHex")] public string LuidHex { get; set; } = "";
    [JsonPropertyName("dedicatedVideoMemoryMb")] public ulong DedicatedVideoMemoryMb { get; set; }
    [JsonPropertyName("isDiscrete")] public bool IsDiscrete { get; set; }
    [JsonPropertyName("isSelected")] public bool IsSelected { get; set; }

    public string Describe() => $"{Description}{(IsDiscrete ? " (discrete)" : "")} - {DedicatedVideoMemoryMb / 1024} MiB VRAM{(IsSelected ? " - selected" : "")}";
}

public sealed class DiagnosticEntry
{
    [JsonPropertyName("timestampUnixMs")] public long TimestampUnixMs { get; set; }
    [JsonPropertyName("severity")] public string Severity { get; set; } = "Info";
    [JsonPropertyName("category")] public string Category { get; set; } = "";
    [JsonPropertyName("message")] public string Message { get; set; } = "";

    public string Describe() => $"[{Severity}] {Category}: {Message}";
}

public sealed class DiagnosticsSnapshot
{
    [JsonPropertyName("selectedAdapterName")] public string SelectedAdapterName { get; set; } = "";
    [JsonPropertyName("selectedAdapterLuid")] public string SelectedAdapterLuid { get; set; } = "";
    [JsonPropertyName("featureLevel")] public string FeatureLevel { get; set; } = "";
    [JsonPropertyName("shaderModel")] public string ShaderModel { get; set; } = "";
    [JsonPropertyName("compositorClockStatus")] public string CompositorClockStatus { get; set; } = "";
    [JsonPropertyName("activeMonitorRefreshHz")] public int ActiveMonitorRefreshHz { get; set; }
    [JsonPropertyName("activeMonitorDpi")] public int ActiveMonitorDpi { get; set; }
    [JsonPropertyName("privateBytesMib")] public double PrivateBytesMib { get; set; }
    [JsonPropertyName("workingSetMib")] public double WorkingSetMib { get; set; }
    [JsonPropertyName("handleCount")] public int HandleCount { get; set; }
    [JsonPropertyName("totalSubmittedFrames")] public ulong TotalSubmittedFrames { get; set; }
    [JsonPropertyName("totalActivations")] public ulong TotalActivations { get; set; }
    [JsonPropertyName("hookReinstallCount")] public int HookReinstallCount { get; set; }
    [JsonPropertyName("keyboardHookInstalled")] public bool KeyboardHookInstalled { get; set; }
    [JsonPropertyName("mouseHookInstalled")] public bool MouseHookInstalled { get; set; }
    [JsonPropertyName("ctrlAltSpaceAvailable")] public bool CtrlAltSpaceAvailable { get; set; }
    [JsonPropertyName("paused")] public bool Paused { get; set; }
    [JsonPropertyName("metrics")] public MetricsSummary Metrics { get; set; } = new();
    [JsonPropertyName("monitors")] public List<MonitorInfo> Monitors { get; set; } = new();
    [JsonPropertyName("adapters")] public List<AdapterInfo> Adapters { get; set; } = new();
    [JsonPropertyName("recentEntries")] public List<DiagnosticEntry> RecentEntries { get; set; } = new();
}
