using System.Text.Json;
using Keychain.Settings.Models;

namespace Keychain.Settings.Services;

public sealed class ConfigDraft
{
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    private ConfigDocument _applied;

    public ConfigDocument Current { get; private set; }
    public event Action? Changed;

    public ConfigDraft(ConfigDocument loaded)
    {
        _applied = loaded.Clone();
        Current = loaded.Clone();
    }

    public bool IsDirty
    {
        get
        {
            var a = JsonSerializer.Serialize(_applied, JsonOptions);
            var c = JsonSerializer.Serialize(Current, JsonOptions);
            return a != c;
        }
    }

    public void RaiseChanged() => Changed?.Invoke();

    public void MarkApplied()
    {
        _applied = Current.Clone();
        RaiseChanged();
    }

    public void ResetToApplied()
    {
        Current = _applied.Clone();
        RaiseChanged();
    }

    public void ReplaceImported(ConfigDocument imported)
    {
        imported.Revision = Current.Revision;
        Current = imported;
        RaiseChanged();
    }

    public List<string> Validate()
    {
        var errors = new List<string>();
        var p = Current.Preferences;

        if (p.RingDiameterDip < 260 || p.RingDiameterDip > 540) errors.Add("Ring diameter must be between 260 and 540 DIPs.");
        if (p.DeadZoneRadiusDip < 24 || p.DeadZoneRadiusDip > 80) errors.Add("Dead zone radius must be between 24 and 80 DIPs.");
        if (p.ChildRingDwellMs < 100 || p.ChildRingDwellMs > 1000) errors.Add("Child-ring dwell must be between 100 and 1000 ms.");
        if (p.SectorGapDip < 1 || p.SectorGapDip > 10) errors.Add("Sector gap must be between 1 and 10 DIPs.");
        if (p.GlassOpacity < 0.35 || p.GlassOpacity > 1.0) errors.Add("Glass opacity must be between 0.35 and 1.0.");
        if (p.IconSizeDip < 16 || p.IconSizeDip > 40) errors.Add("Icon size must be between 16 and 40 DIPs.");
        if (p.AnimationSpeedMultiplier < 0.5 || p.AnimationSpeedMultiplier > 2.0) errors.Add("Animation speed must be between 0.5 and 2.0.");
        if (p.EfficiencyFrameCapHz < 30 || p.EfficiencyFrameCapHz > 240) errors.Add("Efficiency frame cap must be between 30 and 240 Hz.");
        if (p.ActivationMode is not ("WinCaps" or "CtrlAltSpace")) errors.Add("Activation mode must be WinCaps or CtrlAltSpace.");
        if (p.InteractionMode is not ("Hold" or "Toggle")) errors.Add("Interaction mode must be Hold or Toggle.");
        if (p.Theme is not ("Dark" or "Light" or "System")) errors.Add("Theme must be Dark, Light, or System.");
        if (p.MotionPreset is not ("Standard" or "Snappy" or "Gentle")) errors.Add("Motion preset must be Standard, Snappy, or Gentle.");
        if (p.QualityPreset is not ("Auto" or "Performance" or "Efficiency")) errors.Add("Quality preset must be Auto, Performance, or Efficiency.");
        if (p.ReducedMotionOverride is not ("System" or "On" or "Off")) errors.Add("Reduced motion override must be System, On, or Off.");
        if (p.ReducedTransparencyOverride is not ("System" or "On" or "Off")) errors.Add("Reduced transparency override must be System, On, or Off.");

        if (Current.Profiles.Count == 0) errors.Add("At least one profile is required.");
        if (Current.Profiles.Count > 32) errors.Add("Too many profiles (limit 32).");
        if (Current.Rings.Count > 128) errors.Add("Too many rings (limit 128).");
        if (Current.Actions.Count > 512) errors.Add("Too many actions (limit 512).");

        var profileIds = new HashSet<string>();
        foreach (var profile in Current.Profiles)
        {
            if (string.IsNullOrWhiteSpace(profile.Id)) errors.Add("Every profile needs an ID.");
            if (!profileIds.Add(profile.Id)) errors.Add($"Duplicate profile ID: {profile.Id}");
            if (Current.FindRing(profile.RootRingId) is null) errors.Add($"Profile '{profile.Name}' references missing root ring '{profile.RootRingId}'.");
        }
        if (Current.Profiles.All(prof => prof.Id != Current.ActiveProfileId)) errors.Add("The active profile ID does not match any profile.");

        var ringIds = new HashSet<string>();
        foreach (var ring in Current.Rings)
        {
            if (string.IsNullOrWhiteSpace(ring.Id)) errors.Add("Every ring needs an ID.");
            if (!ringIds.Add(ring.Id)) errors.Add($"Duplicate ring ID: {ring.Id}");
            if (ring.SectorCount is not (6 or 8)) errors.Add($"Ring '{ring.Name}' must have 6 or 8 sectors.");
            if (ring.SectorActionIds.Count != ring.SectorCount)
                errors.Add($"Ring '{ring.Name}' has {ring.SectorActionIds.Count} sector assignments but needs exactly {ring.SectorCount}.");
            if (ring.IsChildRing && Current.FindRing(ring.ParentRingId) is null)
                errors.Add($"Child ring '{ring.Name}' references missing parent '{ring.ParentRingId}'.");
        }

        foreach (var ring in Current.Rings)
        {
            if (!ring.IsChildRing) continue;
            if (Current.FindRing(ring.ParentRingId) is { } parent && parent.IsChildRing)
                errors.Add($"Ring '{ring.Name}' nests deeper than one child level.");
            var visited = new HashSet<string>();
            var cursor = ring.Id;
            while (!string.IsNullOrEmpty(cursor) && Current.FindRing(cursor) is { } walk)
            {
                if (!visited.Add(walk.Id)) { errors.Add($"Child-ring cycle detected at '{walk.Id}'."); break; }
                cursor = walk.ParentRingId;
            }
        }

        var actionIds = new HashSet<string>();
        foreach (var action in Current.Actions)
        {
            if (string.IsNullOrWhiteSpace(action.Id)) errors.Add("Every action needs an ID.");
            if (!actionIds.Add(action.Id)) errors.Add($"Duplicate action ID: {action.Id}");
            if (action.Label.Length > 128) errors.Add($"Action label too long: '{action.Label}'");
            if (action.Type is "LaunchApp" or "OpenFile" or "OpenFolder" && string.IsNullOrWhiteSpace(action.Target))
                errors.Add($"Action '{action.Label}' needs a target path.");
            if (action.Type is "LaunchApp" or "OpenFile" &&
                action.Target.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && !action.Approved)
                errors.Add($"Executable action '{action.Label}' is not approved. Review it in the Actions page first.");
            if (action.Type == "OpenUrl")
            {
                if (!action.Target.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
                    !action.Target.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                    errors.Add($"Action '{action.Label}' must use an http:// or https:// URL.");
            }
            if (action.Type == "SendShortcut" && string.IsNullOrWhiteSpace(action.ShortcutChord))
                errors.Add($"Shortcut action '{action.Label}' needs a key chord.");
            if (action.Type == "OpenChildRing" && Current.FindRing(action.ChildRingId) is null)
                errors.Add($"Action '{action.Label}' references missing child ring '{action.ChildRingId}'.");
        }

        foreach (var ring in Current.Rings)
        {
            for (int i = 0; i < ring.SectorActionIds.Count; i++)
            {
                var id = ring.SectorActionIds[i];
                if (string.IsNullOrEmpty(id)) continue;
                if (Current.FindAction(id) is null)
                    errors.Add($"Ring '{ring.Name}' sector {i + 1} references missing action '{id}'.");
            }
        }

        return errors;
    }
}
