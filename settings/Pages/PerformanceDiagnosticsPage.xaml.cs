using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Keychain.Settings.Models;
using Keychain.Settings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using Windows.Storage;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace Keychain.Settings.Pages
{
    public sealed partial class PerformanceDiagnosticsPage : Page
    {
        private bool _loading;

        private ConfigDraft Draft => App.Instance.Draft;
        private MainWindow? Host => App.Instance.MainWindow as MainWindow;

        public PerformanceDiagnosticsPage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            Draft.Changed += OnDraftChanged;
            LoadFromDraft();
            _ = RefreshDiagnosticsAsync(showErrors: false);
            base.OnNavigatedTo(e);
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            Draft.Changed -= OnDraftChanged;
            base.OnNavigatedFrom(e);
        }

        private void OnDraftChanged()
        {
            if (!DispatcherQueue.HasThreadAccess)
            {
                DispatcherQueue.TryEnqueue(LoadFromDraft);
            }
            else
            {
                LoadFromDraft();
            }
        }

        private void Commit()
        {
            Draft.RaiseChanged();
            App.Instance.Preview.NotifyDraftChanged();
        }

        private void LoadFromDraft()
        {
            _loading = true;
            try
            {
                var prefs = Draft.Current.Preferences;
                foreach (ComboBoxItem item in QualityPresetCombo.Items)
                {
                    if ((string)(item.Tag ?? "") == prefs.QualityPreset)
                    {
                        QualityPresetCombo.SelectedItem = item;
                        break;
                    }
                }
                EfficiencyFrameCap.Value = Math.Clamp(prefs.EfficiencyFrameCapHz, 30, 240);
                EfficiencyFrameCap.Visibility = prefs.QualityPreset == "Efficiency" ? Visibility.Visible : Visibility.Collapsed;
                RebuildAdapterCombo(App.Instance.LastDiagnostics, prefs.PreferredAdapterLuid);
                ForegroundExclusionsBox.Text = string.Join(Environment.NewLine, prefs.ForegroundExclusions);
                RedactPathsToggle.IsOn = prefs.RedactPathsInDiagnostics;
            }
            finally
            {
                _loading = false;
            }
        }

        private void RebuildAdapterCombo(DiagnosticsSnapshot? snap, string preferredLuid)
        {
            AdapterCombo.Items.Clear();
            AdapterCombo.Items.Add(new ComboBoxItem { Content = "Auto (display-driving, power-efficient)", Tag = "" });
            int selection = 0;
            if (snap != null)
            {
                foreach (var adapter in snap.Adapters)
                {
                    var item = new ComboBoxItem { Content = adapter.Describe(), Tag = adapter.LuidHex };
                    AdapterCombo.Items.Add(item);
                    if (!string.IsNullOrEmpty(preferredLuid) && adapter.LuidHex == preferredLuid)
                    {
                        selection = AdapterCombo.Items.Count - 1;
                    }
                }
            }
            AdapterCombo.SelectedIndex = selection;
        }

        private void OnQualityPresetChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            if (QualityPresetCombo.SelectedItem is ComboBoxItem item)
            {
                Draft.Current.Preferences.QualityPreset = (string)item.Tag;
            }
            EfficiencyFrameCap.Visibility =
                Draft.Current.Preferences.QualityPreset == "Efficiency" ? Visibility.Visible : Visibility.Collapsed;
            Commit();
        }

        private void OnFrameCapChanged(object sender, Microsoft.UI.Xaml.Controls.NumberBoxValueChangedEventArgs e)
        {
            if (_loading) return;
            Draft.Current.Preferences.EfficiencyFrameCapHz = Math.Clamp((int)Math.Round(e.NewValue), 30, 240);
            Commit();
        }

        private void OnAdapterChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            if (AdapterCombo.SelectedItem is ComboBoxItem item)
            {
                Draft.Current.Preferences.PreferredAdapterLuid = (string)(item.Tag ?? "");
                Commit();
            }
        }

        private void OnExclusionsChanged(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            var lines = ForegroundExclusionsBox.Text
                .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(s => s.Trim())
                .Where(s => s.Length > 0)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();
            Draft.Current.Preferences.ForegroundExclusions = lines;
            Commit();
        }

        private void OnRedactChanged(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            Draft.Current.Preferences.RedactPathsInDiagnostics = RedactPathsToggle.IsOn;
            Commit();
        }

        private async Task RefreshDiagnosticsAsync(bool showErrors)
        {
            try
            {
                var snap = await App.Instance.Ipc.GetDiagnosticsAsync();
                if (snap is null)
                {
                    return;
                }
                App.Instance.LastDiagnostics = snap;
                FillDiagnostics(snap);
                if (!_loading)
                {
                    RebuildAdapterCombo(snap, Draft.Current.Preferences.PreferredAdapterLuid);
                }
            }
            catch (Exception ex)
            {
                if (showErrors)
                {
                    Host?.Show(InfoBarSeverity.Error, $"Diagnostics refresh failed: {ex.Message}");
                }
            }
        }

        private void FillDiagnostics(DiagnosticsSnapshot s)
        {
            StatusGrid.Children.Clear();
            void Row(string label, object value)
            {
                var line = new TextBlock { TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
                line.Text = $"{label}: {value}";
                StatusGrid.Children.Add(line);
            }
            Row("Selected adapter", string.IsNullOrEmpty(s.SelectedAdapterName) ? "(renderer not initialized)" : s.SelectedAdapterName);
            Row("Feature level", s.FeatureLevel);
            Row("Shader model", s.ShaderModel);
            Row("Compositor clock", s.CompositorClockStatus);
            Row("Active monitor", $"{s.ActiveMonitorRefreshHz} Hz at {s.ActiveMonitorDpi} DPI");
            Row("Memory", $"{s.PrivateBytesMib:F1} MiB private, {s.WorkingSetMib:F1} MiB working set");
            Row("Handles", s.HandleCount);
            Row("Submitted frames", s.TotalSubmittedFrames);
            Row("Activations", s.TotalActivations);
            Row("Hook reinstalls", s.HookReinstallCount);
            Row("Keyboard hook installed", s.KeyboardHookInstalled);
            Row("Mouse hook installed", s.MouseHookInstalled);
            Row("Ctrl+Alt+Space available", s.CtrlAltSpaceAvailable);
            Row("Input paused", s.Paused);

            MetricsText.Text =
                $"Hotkey to first submission: {s.Metrics.HotkeyToFirstSubmitMs.Describe()}\n" +
                $"Active-frame CPU work: {s.Metrics.CpuFrameWorkMs.Describe()}\n" +
                $"Active-frame GPU work: {s.Metrics.GpuFrameWorkMs.Describe()}\n" +
                $"Active frame interval: {s.Metrics.ActiveFrameIntervalMs.Describe()}";

            MonitorsList.Items.Clear();
            foreach (var m in s.Monitors)
            {
                MonitorsList.Items.Add(m.Describe());
            }
            AdaptersList.Items.Clear();
            foreach (var a in s.Adapters)
            {
                AdaptersList.Items.Add(a.Describe());
            }
            EntriesList.Items.Clear();
            foreach (var entry in s.RecentEntries.Take(50).Reverse())
            {
                EntriesList.Items.Add(entry.Describe());
            }
        }

        private async void OnRefreshClick(object sender, RoutedEventArgs e)
        {
            RefreshButton.IsEnabled = false;
            try
            {
                await RefreshDiagnosticsAsync(showErrors: true);
            }
            finally
            {
                RefreshButton.IsEnabled = true;
            }
        }

        private async void OnExportClick(object sender, RoutedEventArgs e)
        {
            var picker = new FileSavePicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary, SuggestedFileName = "keychain-diagnostics" };
            picker.FileTypeChoices.Add("Diagnostics report", new List<string> { ".json" });
            InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSaveFileAsync();
            if (file == null) return;
            try
            {
                await App.Instance.Ipc.ExportDiagnosticsAsync(file.Path, includeRawSamples: true);
                Host?.Show(InfoBarSeverity.Success, "Diagnostics report exported.");
            }
            catch (Exception ex)
            {
                Host?.Show(InfoBarSeverity.Error, $"Export failed: {ex.Message}");
            }
        }

        private async void OnResetInputClick(object sender, RoutedEventArgs e)
        {
            try
            {
                await App.Instance.Ipc.ResetInputAsync();
                Host?.Show(InfoBarSeverity.Success, "Input hooks were reset and reinstalled.");
                await RefreshDiagnosticsAsync(showErrors: false);
            }
            catch (Exception ex)
            {
                Host?.Show(InfoBarSeverity.Error, $"Reset failed: {ex.Message}");
            }
        }
    }
}
