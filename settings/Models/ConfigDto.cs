using System.Text.Json;
using System.Text.Json.Serialization;

namespace Keychain.Settings.Models;

public sealed class Preferences
{
    [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
    [JsonPropertyName("launchAtStartup")] public bool LaunchAtStartup { get; set; }
    [JsonPropertyName("showTrayIcon")] public bool ShowTrayIcon { get; set; } = true;
    [JsonPropertyName("completedTutorial")] public bool CompletedTutorial { get; set; }
    [JsonPropertyName("activationMode")] public string ActivationMode { get; set; } = "WinCaps";
    [JsonPropertyName("interactionMode")] public string InteractionMode { get; set; } = "Hold";
    [JsonPropertyName("ringDiameterDip")] public double RingDiameterDip { get; set; } = 380.0;
    [JsonPropertyName("defaultSectorCount")] public int DefaultSectorCount { get; set; } = 6;
    [JsonPropertyName("deadZoneRadiusDip")] public double DeadZoneRadiusDip { get; set; } = 44.0;
    [JsonPropertyName("childRingDwellMs")] public int ChildRingDwellMs { get; set; } = 250;
    [JsonPropertyName("sectorGapDip")] public double SectorGapDip { get; set; } = 3.5;
    [JsonPropertyName("theme")] public string Theme { get; set; } = "Dark";
    [JsonPropertyName("accentColorHex")] public string AccentColorHex { get; set; } = "#5CC8FF";
    [JsonPropertyName("glassOpacity")] public double GlassOpacity { get; set; } = 0.86;
    [JsonPropertyName("iconSizeDip")] public double IconSizeDip { get; set; } = 24.0;
    [JsonPropertyName("motionPreset")] public string MotionPreset { get; set; } = "Standard";
    [JsonPropertyName("animationSpeedMultiplier")] public double AnimationSpeedMultiplier { get; set; } = 1.0;
    [JsonPropertyName("reducedMotionOverride")] public string ReducedMotionOverride { get; set; } = "System";
    [JsonPropertyName("reducedTransparencyOverride")] public string ReducedTransparencyOverride { get; set; } = "System";
    [JsonPropertyName("qualityPreset")] public string QualityPreset { get; set; } = "Auto";
    [JsonPropertyName("efficiencyFrameCapHz")] public int EfficiencyFrameCapHz { get; set; } = 60;
    [JsonPropertyName("preferredAdapterLuid")] public string PreferredAdapterLuid { get; set; } = "";
    [JsonPropertyName("redactPathsInDiagnostics")] public bool RedactPathsInDiagnostics { get; set; } = true;
    [JsonPropertyName("foregroundExclusions")] public List<string> ForegroundExclusions { get; set; } = new();

    public Preferences Clone() => JsonSerializer.Deserialize<Preferences>(JsonSerializer.Serialize(this))!;
}

public sealed class Profile
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("rootRingId")] public string RootRingId { get; set; } = "";
    [JsonPropertyName("exclusions")] public List<string> Exclusions { get; set; } = new();
}

public sealed class RadialRing
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("sectorCount")] public int SectorCount { get; set; } = 6;
    [JsonPropertyName("isChildRing")] public bool IsChildRing { get; set; }
    [JsonPropertyName("parentRingId")] public string ParentRingId { get; set; } = "";
    [JsonPropertyName("sectorActionIds")] public List<string> SectorActionIds { get; set; } = new();
}

public sealed class ActionDefinition
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("type")] public string Type { get; set; } = "None";
    [JsonPropertyName("label")] public string Label { get; set; } = "";
    [JsonPropertyName("target")] public string Target { get; set; } = "";
    [JsonPropertyName("arguments")] public string Arguments { get; set; } = "";
    [JsonPropertyName("workingDirectory")] public string WorkingDirectory { get; set; } = "";
    [JsonPropertyName("shortcutChord")] public string ShortcutChord { get; set; } = "";
    [JsonPropertyName("windowsActionName")] public string WindowsActionName { get; set; } = "";
    [JsonPropertyName("childRingId")] public string ChildRingId { get; set; } = "";
    [JsonPropertyName("iconGlyph")] public string IconGlyph { get; set; } = "";
    [JsonPropertyName("approved")] public bool Approved { get; set; } = true;
    [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
}

public sealed class ConfigDocument
{
    [JsonPropertyName("schemaVersion")] public int SchemaVersion { get; set; } = 1;
    [JsonPropertyName("revision")] public ulong Revision { get; set; } = 1;
    [JsonPropertyName("activeProfileId")] public string ActiveProfileId { get; set; } = "profile_default";
    [JsonPropertyName("preferences")] public Preferences Preferences { get; set; } = new();
    [JsonPropertyName("profiles")] public List<Profile> Profiles { get; set; } = new();
    [JsonPropertyName("rings")] public List<RadialRing> Rings { get; set; } = new();
    [JsonPropertyName("actions")] public List<ActionDefinition> Actions { get; set; } = new();

    [JsonIgnore] public Profile? ActiveProfile => Profiles.FirstOrDefault(p => p.Id == ActiveProfileId);
    [JsonIgnore] public RadialRing? ActiveRootRing => ActiveProfile is { } prof ? FindRing(prof.RootRingId) : null;

    public RadialRing? FindRing(string id) => Rings.FirstOrDefault(r => r.Id == id);
    public ActionDefinition? FindAction(string id) => Actions.FirstOrDefault(a => a.Id == id);

    public ConfigDocument Clone() =>
        JsonSerializer.Deserialize<ConfigDocument>(JsonSerializer.Serialize(this)) ?? new ConfigDocument();

    public static ConfigDocument CreateFallbackDefault() => DefaultConfigFactory.Create();
}

public static class DefaultConfigFactory
{
    public static ConfigDocument Create()
    {
        ActionDefinition Action(string id, string type, string label, string glyph, string windowsAction = "") => new()
        {
            Id = id, Type = type, Label = label, IconGlyph = glyph, WindowsActionName = windowsAction
        };

        var doc = new ConfigDocument();
        doc.Actions.Add(Action("act_documents", "WindowsAction", "Documents", "DOC", "Documents"));
        doc.Actions.Add(Action("act_downloads", "WindowsAction", "Downloads", "DL", "Downloads"));
        doc.Actions.Add(Action("act_explorer", "WindowsAction", "File Explorer", "EXP", "FileExplorer"));
        doc.Actions.Add(Action("act_notepad", "WindowsAction", "Notepad", "TXT", "Notepad"));
        doc.Actions.Add(Action("act_win_settings", "WindowsAction", "Windows Settings", "WIN", "WindowsSettings"));
        doc.Actions.Add(Action("act_keychain_settings", "OpenKeychainSettings", "Keychain Settings", "CFG"));
        doc.Actions.Add(Action("act_task_manager", "WindowsAction", "Task Manager", "TSK", "TaskManager"));
        doc.Actions.Add(new ActionDefinition { Id = "act_copy_chord", Type = "SendShortcut", Label = "Copy Selection", IconGlyph = "CPY", ShortcutChord = "Ctrl+C" });
        doc.Actions.Add(new ActionDefinition { Id = "act_paste_chord", Type = "SendShortcut", Label = "Paste Clipboard", IconGlyph = "PST", ShortcutChord = "Ctrl+V" });

        doc.Rings.Add(new RadialRing
        {
            Id = "ring_root_default", Name = "Main Ring", SectorCount = 6,
            SectorActionIds = new List<string> { "act_documents", "act_downloads", "act_explorer", "act_notepad", "act_win_settings", "act_keychain_settings" }
        });
        doc.Rings.Add(new RadialRing
        {
            Id = "ring_quick_tools", Name = "Quick Tools", SectorCount = 6, IsChildRing = true, ParentRingId = "ring_root_default",
            SectorActionIds = new List<string> { "act_task_manager", "act_copy_chord", "act_paste_chord", "act_documents", "act_explorer", "act_keychain_settings" }
        });

        doc.Profiles.Add(new Profile { Id = "profile_default", Name = "Default Profile", RootRingId = "ring_root_default" });
        doc.ActiveProfileId = "profile_default";
        return doc;
    }
}
