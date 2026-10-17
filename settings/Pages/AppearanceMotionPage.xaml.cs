using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Keychain.Settings.Pages;

public sealed partial class AppearanceMotionPage : Page {
    private bool _loading;

    public AppearanceMotionPage() {
        InitializeComponent();
        Loaded += (_, _) => LoadFromDraft();
        App.Instance.Draft.Changed += OnDraftChanged;
    }

    private void OnDraftChanged() {
        if (!DispatcherQueue.HasThreadAccess) {
            _ = DispatcherQueue.TryEnqueue(LoadFromDraft);
            return;
        }
        LoadFromDraft();
    }

    private static Models.Preferences Prefs => App.Instance.Draft.Current.Preferences;

    private static void Commit() {
        var draft = App.Instance.Draft;
        draft.RaiseChanged();
        App.Instance.Preview.NotifyDraftChanged();
    }

    private void LoadFromDraft() {
        if (_loading) {
            return;
        }
        _loading = true;
        try {
            var p = Prefs;
            ThemeDarkRadio.SelectedIndex = p.Theme == "Dark" ? 0 : -1;
            ThemeLightRadio.SelectedIndex = p.Theme == "Light" ? 0 : -1;
            ThemeSystemRadio.SelectedIndex = p.Theme == "System" ? 0 : -1;

            AccentColorPicker.Color = HexToColor(p.AccentColorHex, Color.FromArgb(255, 0x5C, 0xC8, 0xFF));
            GlassOpacitySlider.Value = p.GlassOpacity;
            IconSizeSlider.Value = p.IconSizeDip;

            MotionStandardRadio.SelectedIndex = p.MotionPreset == "Standard" ? 0 : -1;
            MotionSnappyRadio.SelectedIndex = p.MotionPreset == "Snappy" ? 0 : -1;
            MotionGentleRadio.SelectedIndex = p.MotionPreset == "Gentle" ? 0 : -1;
            AnimationSpeedSlider.Value = p.AnimationSpeedMultiplier;

            ReducedMotionCombo.SelectedIndex = p.ReducedMotionOverride switch {
                "On" => 1,
                "Off" => 2,
                _ => 0,
            };
            ReducedTransparencyCombo.SelectedIndex = p.ReducedTransparencyOverride switch {
                "On" => 1,
                "Off" => 2,
                _ => 0,
            };
        } finally {
            _loading = false;
        }
    }

    private static Color HexToColor(string hex, Color fallback) {
        if (string.IsNullOrWhiteSpace(hex) || hex.Length != 7 || hex[0] != '#') {
            return fallback;
        }
        try {
            var r = Convert.ToByte(hex.Substring(1, 2), 16);
            var g = Convert.ToByte(hex.Substring(3, 2), 16);
            var b = Convert.ToByte(hex.Substring(5, 2), 16);
            return Color.FromArgb(255, r, g, b);
        } catch (FormatException) {
            return fallback;
        }
    }

    private static string ColorToHex(Color c) =>
        $"#{c.R:X2}{c.G:X2}{c.B:X2}";

    private void OnThemeChanged(object sender, SelectionChangedEventArgs e) {
        if (_loading) {
            return;
        }
        var radio = sender as RadioButtons;
        if (radio?.SelectedItem is not string value) {
            return;
        }
        Prefs.Theme = value;
        Commit();
    }

    private void OnAccentColorChanged(ColorPicker sender, ColorChangedEventArgs args) {
        if (_loading) {
            return;
        }
        Prefs.AccentColorHex = ColorToHex(args.NewColor);
        Commit();
    }

    private void OnGlassOpacityChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e) {
        if (_loading) {
            return;
        }
        Prefs.GlassOpacity = (float)e.NewValue;
        Commit();
    }

    private void OnIconSizeChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e) {
        if (_loading) {
            return;
        }
        Prefs.IconSizeDip = (float)Math.Round(e.NewValue);
        Commit();
    }

    private void OnMotionPresetChanged(object sender, SelectionChangedEventArgs e) {
        if (_loading) {
            return;
        }
        var radio = sender as RadioButtons;
        if (radio?.SelectedItem is not string value) {
            return;
        }
        Prefs.MotionPreset = value;
        Commit();
    }

    private void OnAnimationSpeedChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e) {
        if (_loading) {
            return;
        }
        Prefs.AnimationSpeedMultiplier = (float)Math.Round(e.NewValue, 2);
        Commit();
    }

    private void OnReducedMotionOverrideChanged(object sender, SelectionChangedEventArgs e) {
        if (_loading) {
            return;
        }
        if ((sender as ComboBox)?.SelectedItem is ComboBoxItem item && item.Tag is string tag) {
            Prefs.ReducedMotionOverride = tag;
            Commit();
        }
    }

    private void OnReducedTransparencyOverrideChanged(object sender, SelectionChangedEventArgs e) {
        if (_loading) {
            return;
        }
        if ((sender as ComboBox)?.SelectedItem is ComboBoxItem item && item.Tag is string tag) {
            Prefs.ReducedTransparencyOverride = tag;
            Commit();
        }
    }

    private void OnResetAppearanceClick(object sender, RoutedEventArgs e) {
        var p = Prefs;
        p.Theme = "Dark";
        p.AccentColorHex = "#5CC8FF";
        p.GlassOpacity = 0.86f;
        p.IconSizeDip = 24.0f;
        p.MotionPreset = "Standard";
        p.AnimationSpeedMultiplier = 1.0f;
        p.ReducedMotionOverride = "System";
        p.ReducedTransparencyOverride = "System";
        LoadFromDraft();
        Commit();
    }
}
