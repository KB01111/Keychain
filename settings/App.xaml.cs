using Keychain.Settings.Models;
using Keychain.Settings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace Keychain.Settings;

public sealed partial class App : Application
{
    public static App Instance => (App)Current;
    public MainWindow? MainWindow { get; private set; }
    public IpcClient Ipc { get; } = new IpcClient();
    public PreviewController Preview { get; } = new();
    public ConfigDraft Draft { get; private set; } = null!;
    public DiagnosticsSnapshot? LastDiagnostics { get; set; }
    public static nint MainWindowHandle { get; set; }

    public App()
    {
        InitializeComponent();
        UnhandledException += OnUnhandledException;
    }

    private void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        try
        {
            var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Keychain");
            System.IO.Directory.CreateDirectory(dir);
            System.IO.File.AppendAllText(
                System.IO.Path.Combine(dir, "settings-errors.log"),
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {e.Message}{Environment.NewLine}{e.Exception}{Environment.NewLine}{new string('-', 60)}{Environment.NewLine}");
        }
        catch
        {
        }
        System.Diagnostics.Debug.WriteLine("Unhandled: " + e.Message);
        e.Handled = true;
    }

    protected override async void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
    {
        ConfigDocument? loaded = null;
        try
        {
            loaded = await Ipc.GetConfigAsync();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine("GetConfig failed: " + ex.Message);
        }

        if (loaded == null)
        {
            Draft = new ConfigDraft(ConfigDocument.CreateFallbackDefault());
            MainWindow = new MainWindow(canOperate: false);
            MainWindow.Activate();
            return;
        }

        Draft = new ConfigDraft(loaded);
        MainWindow = new MainWindow(canOperate: true);
        MainWindow.Activate();
    }

    public static void ApplyMicaBackdrop(Microsoft.UI.Xaml.Window window)
    {
        try
        {
            window.SystemBackdrop = new MicaBackdrop();
        }
        catch
        {
            window.SystemBackdrop = null;
        }
    }
}
