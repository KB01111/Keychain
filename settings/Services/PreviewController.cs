using Microsoft.UI.Dispatching;

namespace Keychain.Settings.Services;

public sealed class PreviewController
{
    private bool _active;
    private DispatcherQueueTimer? _debounce;

    public bool IsActive => _active;
    public event Action? StateChanged;

    public async System.Threading.Tasks.Task StartAsync()
    {
        try
        {
            _active = await App.Instance.Ipc.PreviewStartAsync(App.Instance.Draft.Current);
        }
        catch
        {
            _active = false;
        }
        StateChanged?.Invoke();
    }

    public async System.Threading.Tasks.Task StopAsync()
    {
        _debounce?.Stop();
        if (!_active) return;
        try
        {
            await App.Instance.Ipc.PreviewStopAsync();
        }
        catch
        {
        }
        _active = false;
        StateChanged?.Invoke();
    }

    public void NotifyDraftChanged()
    {
        if (!_active) return;
        if (_debounce is null)
        {
            _debounce = DispatcherQueue.GetForCurrentThread()?.CreateTimer();
            if (_debounce is null) return;
            _debounce.Interval = TimeSpan.FromMilliseconds(250);
            _debounce.IsRepeating = false;
            _debounce.Tick += async (_, _) =>
            {
                try
                {
                    _active = await App.Instance.Ipc.PreviewUpdateAsync(App.Instance.Draft.Current);
                }
                catch
                {
                    _active = false;
                }
                StateChanged?.Invoke();
            };
        }
        _debounce.Stop();
        _debounce.Start();
    }

    public async System.Threading.Tasks.Task StopFromWindowAsync()
    {
        await StopAsync();
    }
}
