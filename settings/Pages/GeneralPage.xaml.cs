using Keychain.Settings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Keychain.Settings.Pages;

public sealed partial class GeneralPage : Page
{
    private bool _loading;
    private bool _pauseBusy;

    public GeneralPage()
    {
        InitializeComponent();
        App.Instance.Draft.Changed += OnDraftChanged;
        Loaded += (_, _) => LoadFromDraft();
    }

    private void OnDraftChanged() => DispatcherQueue.TryEnqueue(LoadFromDraft);

    private void LoadFromDraft()
    {
        if (_loading) return;
        _loading = true;
        var p = App.Instance.Draft.Current.Preferences;
        EnabledSwitch.IsOn = p.Enabled;
        StartupSwitch.IsOn = p.LaunchAtStartup;
        TraySwitch.IsOn = p.ShowTrayIcon;
        TutorialDone.IsChecked = p.CompletedTutorial;
        _loading = false;
    }

    private void OnEnabledToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.Enabled = EnabledSwitch.IsOn;
        App.Instance.Draft.RaiseChanged();
    }

    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.LaunchAtStartup = StartupSwitch.IsOn;
        App.Instance.Draft.RaiseChanged();
    }

    private void OnTrayToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.ShowTrayIcon = TraySwitch.IsOn;
        App.Instance.Draft.RaiseChanged();
    }

    private void OnTutorialDoneToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        App.Instance.Draft.Current.Preferences.CompletedTutorial = TutorialDone.IsChecked == true;
        App.Instance.Draft.RaiseChanged();
    }

    private async void OnPauseNowClick(object sender, RoutedEventArgs e)
    {
        if (_pauseBusy) return;
        _pauseBusy = true;
        BusyRing.IsActive = true;
        BusyRing.Visibility = Visibility.Visible;
        try
        {
            var diag = await App.Instance.Ipc.GetDiagnosticsAsync();
            if (diag is { Paused: true })
            {
                await App.Instance.Ipc.ResumeAsync();
                PauseState.Text = "Hotring input resumed.";
                PauseButton.Content = "Pause hotring input now";
            }
            else
            {
                await App.Instance.Ipc.PauseAsync();
                PauseState.Text = "Hotring input paused. Press the button again to resume.";
                PauseButton.Content = "Resume hotring input";
            }
        }
        catch (Exception ex)
        {
            PauseState.Text = "Failed: " + ex.Message;
        }
        BusyRing.IsActive = false;
        BusyRing.Visibility = Visibility.Collapsed;
        _pauseBusy = false;
    }

    private async void OnTutorialClick(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            Title = "Keychain gesture tutorial",
            Content = "1. Hold the Windows key.\n2. Press and hold Caps Lock - the radial ring opens under the pointer.\n3. Move toward an action; the highlight follows the pointer immediately.\n4. Release Caps Lock over an action to commit. Release in the center to cancel.\n5. Press Escape any time to cancel. Right-click returns from a child ring.\n\nChild rings: hold the pointer over a sub-ring entry (or click) to open its sector ring.\nAlternate activation: Ctrl+Alt+Space can be enabled on the Hotring page.",
            CloseButtonText = "Got it",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = XamlRoot
        };
        await dialog.ShowAsync();
    }
}
