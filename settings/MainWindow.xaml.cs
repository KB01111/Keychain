using Keychain.Settings.Pages;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace Keychain.Settings;

public sealed partial class MainWindow : Window
{
    private readonly bool _canOperate;

    public MainWindow(bool canOperate)
    {
        _canOperate = canOperate;
        InitializeComponent();
        App.MainWindowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        App.ApplyMicaBackdrop(this);
        Title = "Keychain Settings";
        AppWindow.Resize(new Windows.Graphics.SizeInt32 { Width = 1180, Height = 760 });
        if (Content is FrameworkElement root)
        {
            root.RequestedTheme = Microsoft.UI.Xaml.ElementTheme.Default;
        }

        App.Instance.Draft.Changed += OnDraftChanged;

        Closed += OnClosed;

        if (canOperate)
        {
            Nav.SelectedItem = Nav.MenuItems[0];
            ContentFrame.Navigate(typeof(GeneralPage));
        }
        else
        {
            ContentFrame.Navigate(typeof(GeneralPage));
            SetOperationalUi(false);
        }
        UpdateDraftUi();
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _ = App.Instance.Preview.StopFromWindowAsync();
    }

    private void OnDraftChanged()
    {
        DispatcherQueue.TryEnqueue(UpdateDraftUi);
    }

    private void UpdateDraftUi()
    {
        bool dirty = _canOperate && App.Instance.Draft.IsDirty;
        DraftStatus.Text = dirty ? "Unsaved draft - Apply to send to Keychain." : "No pending changes.";
        ApplyButton.IsEnabled = dirty;
        DiscardButton.IsEnabled = dirty;
    }

    private void SetOperationalUi(bool operational)
    {
        Status.Message = operational ? string.Empty
            : "Cannot reach the Keychain launcher. Start Keychain.exe, then reopen Keychain Settings.";
        Status.Severity = operational ? InfoBarSeverity.Informational : InfoBarSeverity.Warning;
        Status.IsOpen = !operational;
        ApplyButton.IsEnabled = false;
        DiscardButton.IsEnabled = false;
    }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem item || item.Tag is not string tag) return;
        var pageType = tag switch
        {
            "general" => typeof(GeneralPage),
            "hotring" => typeof(HotringPage),
            "actions" => typeof(ActionsProfilesPage),
            "appearance" => typeof(AppearanceMotionPage),
            "diagnostics" => typeof(PerformanceDiagnosticsPage),
            _ => typeof(GeneralPage)
        };
        if (ContentFrame.CurrentSourcePageType != pageType)
        {
            ContentFrame.Navigate(pageType);
        }
    }

    private async void OnApplyClick(object sender, RoutedEventArgs e)
    {
        var draft = App.Instance.Draft;
        var errors = draft.Validate();
        if (errors.Count > 0)
        {
            Show(InfoBarSeverity.Error, $"Validation failed: {errors[0]}{(errors.Count > 1 ? $" (and {errors.Count - 1} more)" : "")}");
            return;
        }

        ApplyButton.IsEnabled = false;
        try
        {
            ulong revision = await App.Instance.Ipc.ApplyConfigAsync(draft.Current);
            draft.MarkApplied();
            Show(InfoBarSeverity.Success, $"Configuration applied (revision {revision}).");
        }
        catch (Exception ex)
        {
            Show(InfoBarSeverity.Error, "Apply failed: " + ex.Message);
        }
        UpdateDraftUi();
    }

    private void OnDiscardClick(object sender, RoutedEventArgs e)
    {
        App.Instance.Draft.ResetToApplied();
        Show(InfoBarSeverity.Informational, "Unsaved changes discarded.");
        UpdateDraftUi();
    }

    public void Show(InfoBarSeverity severity, string message)
    {
        Status.Severity = severity;
        Status.Message = message;
        Status.IsOpen = true;
    }
}
