using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Keychain.Settings.Models;

namespace Keychain.Settings.Services;

public sealed class IpcException : Exception
{
    public IpcException(string message) : base(message) { }
}

public sealed class IpcClient
{
    private const int ProtocolVersion = 1;
    private const int MaxMessageBytes = 1024 * 1024;
    private const int DefaultTimeoutMs = 3000;
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    private long _requestCounter;

    public bool IsServerRunning()
    {
        try
        {
            using var probe = new NamedPipeClientStream(
                ".", PipeName, PipeDirection.InOut,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            probe.Connect(250);
            return probe.IsConnected;
        }
        catch
        {
            return false;
        }
    }

    private static string PipeName => $"Keychain.Ipc.v1.{Process.GetCurrentProcess().SessionId}";

    public async Task<JsonObject> SendRawAsync(string type, JsonNode? payload, int timeoutMs = DefaultTimeoutMs)
    {
        var request = new JsonObject
        {
            ["version"] = ProtocolVersion,
            ["requestId"] = $"{Interlocked.Increment(ref _requestCounter)}",
            ["type"] = type,
            ["payload"] = payload?.DeepClone() ?? new JsonObject()
        };

        var requestBytes = Encoding.UTF8.GetBytes(request.ToJsonString());
        if (requestBytes.Length > MaxMessageBytes)
        {
            throw new IpcException("Request exceeds the 1 MiB IPC message limit.");
        }

        await using var pipe = new NamedPipeClientStream(
            ".", PipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(timeoutMs);

        var lenPrefix = BitConverter.GetBytes((uint)requestBytes.Length);
        await pipe.WriteAsync(lenPrefix);
        await pipe.WriteAsync(requestBytes);
        await pipe.FlushAsync();

        var lenBuf = new byte[4];
        await ReadExactAsync(pipe, lenBuf);
        var respLen = BitConverter.ToUInt32(lenBuf, 0);
        if (respLen == 0 || respLen > MaxMessageBytes)
        {
            throw new IpcException("Invalid IPC response length.");
        }

        var respBuf = new byte[respLen];
        await ReadExactAsync(pipe, respBuf);

        var resp = JsonNode.Parse(Encoding.UTF8.GetString(respBuf)) as JsonObject
                   ?? throw new IpcException("Malformed IPC response.");
        if (resp["ok"]?.GetValue<bool>() != true)
        {
            var err = resp["error"]?.GetValue<string>() ?? "unknown IPC error";
            var detail = new List<string>();
            if (resp["errors"] is JsonArray arr)
            {
                foreach (var e in arr)
                {
                    if (e?.GetValue<string>() is { } s) detail.Add(s);
                }
            }
            throw new IpcException(detail.Count > 0 ? $"{err}: {string.Join("; ", detail)}" : err);
        }
        return resp;
    }

    private static async Task ReadExactAsync(PipeStream pipe, byte[] buffer)
    {
        int read = 0;
        while (read < buffer.Length)
        {
            int n = await pipe.ReadAsync(buffer.AsMemory(read, buffer.Length - read));
            if (n <= 0) throw new IpcException("IPC pipe closed unexpectedly.");
            read += n;
        }
    }

    private static JsonNode SerializeNode<T>(T value) =>
        JsonSerializer.SerializeToNode(value, JsonOptions)!;

    public async Task<ConfigDocument?> GetConfigAsync()
    {
        var resp = await SendRawAsync("GetConfig", null);
        return resp["payload"]?.Deserialize<ConfigDocument>(JsonOptions);
    }

    public async Task<ulong> ApplyConfigAsync(ConfigDocument doc)
    {
        var payload = new JsonObject { ["config"] = SerializeNode(doc) };
        var resp = await SendRawAsync("ApplyConfig", payload, 8000);
        var p = resp["payload"]!;
        return p["revision"]?.GetValue<ulong>() ?? doc.Revision;
    }

    public async Task<ConfigDocument?> ImportConfigAsync(ConfigDocument doc)
    {
        var payload = new JsonObject { ["config"] = SerializeNode(doc) };
        var resp = await SendRawAsync("ImportConfig", payload);
        return resp["payload"]?.Deserialize<ConfigDocument>(JsonOptions);
    }

    public async Task<bool> PreviewStartAsync(ConfigDocument doc)
    {
        var payload = new JsonObject { ["config"] = SerializeNode(doc) };
        var resp = await SendRawAsync("PreviewStart", payload);
        return resp["payload"]?["previewActive"]?.GetValue<bool>() ?? false;
    }

    public async Task<bool> PreviewUpdateAsync(ConfigDocument doc)
    {
        var payload = new JsonObject { ["config"] = SerializeNode(doc) };
        var resp = await SendRawAsync("PreviewUpdate", payload);
        return resp["payload"]?["previewActive"]?.GetValue<bool>() ?? false;
    }

    public async Task PreviewStopAsync()
    {
        await SendRawAsync("PreviewStop", null);
    }

    public async Task<DiagnosticsSnapshot?> GetDiagnosticsAsync()
    {
        var resp = await SendRawAsync("GetDiagnostics", null);
        return resp["payload"]?.Deserialize<DiagnosticsSnapshot>(JsonOptions);
    }

    public async Task<bool> ExportDiagnosticsAsync(string utf8Path, bool includeRawSamples)
    {
        var payload = new JsonObject
        {
            ["path"] = utf8Path,
            ["includeRawSamples"] = includeRawSamples
        };
        var resp = await SendRawAsync("ExportDiagnostics", payload, 8000);
        return resp["payload"]?["exported"]?.GetValue<bool>() ?? false;
    }

    public async Task<bool> ResetInputAsync()
    {
        var resp = await SendRawAsync("ResetInput", null);
        return resp["payload"]?["hooksReinstalled"]?.GetValue<bool>() ?? false;
    }

    public async Task<bool> PauseAsync()
    {
        var resp = await SendRawAsync("Pause", null);
        return resp["payload"]?["paused"]?.GetValue<bool>() ?? false;
    }

    public async Task<bool> ResumeAsync()
    {
        var resp = await SendRawAsync("Resume", null);
        return resp["payload"]?["paused"]?.GetValue<bool?>() ?? false;
    }

    public async Task<bool> ShowSettingsAsync()
    {
        var resp = await SendRawAsync("ShowSettings", null);
        return resp["payload"]?["launched"]?.GetValue<bool>() ?? false;
    }
}
