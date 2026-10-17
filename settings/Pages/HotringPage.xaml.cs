using Keychain.Settings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Keychain.Settings.Pages;

public sealed partial class HotringPage : Page
{
    private bool _loading;

    public HotringPage()
    {
        InitializeComponent();
        App.Instance.Preview.StateChanged += OnPreviewStateChanged;
        App.Instance.Draft.Changed += OnDraftChanged;
        Loaded += (_, _) => LoadFromDraft();
    }

    private void OnPreviewStateChanged() =>
        DispatcherQueue.TryEnqueue(() => PreviewToggle.IsChecked = App.Instance.Preview.IsActive);

    private void OnDraftChanged()
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            LoadFromDraft();
            App.Instance.Preview.NotifyDraftChanged();
        });
    }

    private void LoadFromDraft()
    {
        if (_loading) return;
        _loading = true;
        var p = App.Instance.Draft.Current.Preferences;
        ChordWinCaps.IsChecked = p.ActivationMode == "WinCaps";
        ChordCtrlAltSpace.IsChecked = p.ActivationMode == "CtrlAltSpace";
        ModeHold.IsChecked = p.InteractionMode == "Hold";
        ModeToggle.IsChecked = p.InteractionMode == "Toggle";
        Diameter.Value = p.RingDiameterDip;
        DeadZone.Value = p.DeadZoneRadiusDip;
        SectorGap.Value = p.SectorGapDip;
        Dwell.Value = p.ChildRingDwellMs;
        Sectors6.IsChecked = p.DefaultSectorCount == 6;
        Sectors8.IsChecked = p.DefaultSectorCount == 8;
        _loading = false;
    }

    private void MarkChanged()
    {
        App.Instance.Draft.RaiseChanged();
    }

    private void OnChordChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (ChordWinCaps.IsChecked == true) App.Instance.Draft.Current.Preferences.ActivationMode = "WinCaps";
        else if (ChordCtrlAltSpace.IsChecked == true) App.Instance.Draft.Current.Preferences.ActivationMode = "CtrlAltSpace";
        MarkChanged();
    }

    private void OnModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (ModeHold.IsChecked == true) App.Instance.Draft.Current.Preferences.InteractionMode = "Hold";
        else if (ModeToggle.IsChecked == true) App.Instance.Draft.Current.Preferences.InteractionMode = "Toggle";
        MarkChanged();
    }

    private void OnDiameterChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.RingDiameterDip = Diameter.Value;
        MarkChanged();
    }

    private void OnDeadZoneChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.DeadZoneRadiusDip = DeadZone.Value;
        MarkChanged();
    }

    private void OnSectorGapChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.SectorGapDip = SectorGap.Value;
        MarkChanged();
    }

    private void OnDwellChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.ChildRingDwellMs = (int)Dwell.Value;
        MarkChanged();
    }

    private void OnSectorCountChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (Sectors6.IsChecked == true) App.Instance.Draft.Current.Preferences.DefaultSectorCount = 6;
        else if (Sectors8.IsChecked == true) App.Instance.Draft.Current.Preferences.DefaultSectorCount = 8;
        MarkChanged();
    }

    private async void OnPreviewToggle(object sender, RoutedEventArgs e)
    {
        PreviewBusy.IsActive = true;
        PreviewBusy.Visibility = Visibility.Visible;
        try
        {
            if (App.Instance.Preview.IsActive)
            {
                await App.Instance.Preview.StopAsync();
                PageStatus.Severity = InfoBarSeverity.Informational;
                PageStatus.Message = "Preview stopped.";
            }
            else
            {
                await App.Instance.Preview.StartAsync();
                PageStatus.Severity = App.Instance.Preview.IsActive ? InfoBarSeverity.Success : InfoBarSeverity.Error;
                PageStatus.Message = App.Instance.Preview.IsActive
                    ? "Preview active - the real Keychain overlay is showing your draft ring. Actions never execute in preview mode."
                    : "Preview could not start. Is Keychain.exe running?";
                PageStatus.IsOpen = true;
            }
        }
        finally
        {
            PreviewBusy.IsActive = false;
            PreviewBusy.Visibility = Visibility.Collapsed;
            PreviewToggle.IsChecked = App.Instance.Preview.IsActive;
        }
    }
}
