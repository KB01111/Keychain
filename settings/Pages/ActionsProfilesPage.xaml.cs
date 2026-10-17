using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Keychain.Settings.Controls;
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
    public sealed record ActionListItem(string Glyph, string Label, string TypeSummary, string ActionId);

    public sealed partial class ActionsProfilesPage : Page
    {
        private bool _loading;
        private string _selectedRingId = "";
        private int _selectedSector = -1;

        private ConfigDraft Draft => App.Instance.Draft;
        private ConfigDocument Doc => Draft.Current;
        private MainWindow? Host => App.Instance.MainWindow as MainWindow;

        public ActionsProfilesPage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            Draft.Changed += OnDraftChanged;
            RingEditor.SectorSelected += OnSectorSelected;
            LoadFromDraft();
            base.OnNavigatedTo(e);
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            Draft.Changed -= OnDraftChanged;
            RingEditor.SectorSelected -= OnSectorSelected;
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

        private RadialRing? SelectedRing => Doc.Rings.FirstOrDefault(r => r.Id == _selectedRingId);

        private void LoadFromDraft()
        {
            _loading = true;
            try
            {
                RebuildProfiles();
                RebuildRings();
                RebuildActions();
            }
            finally
            {
                _loading = false;
            }
        }

        private void RebuildProfiles()
        {
            int last = ProfileCombo.SelectedIndex;
            ProfileCombo.Items.Clear();
            foreach (var p in Doc.Profiles)
            {
                var item = new ComboBoxItem { Content = p.Name, Tag = p.Id };
                ProfileCombo.Items.Add(item);
                if (p.Id == Doc.ActiveProfileId) ProfileCombo.SelectedItem = item;
            }
            if (ProfileCombo.SelectedItem == null && ProfileCombo.Items.Count > 0)
            {
                ProfileCombo.SelectedIndex = 0;
            }
            if (Doc.ActiveProfile is Profile prof)
            {
                ProfileExclusionsBox.Text = string.Join(Environment.NewLine, prof.Exclusions);
            }
        }

        private void RebuildRings()
        {
            RingCombo.Items.Clear();
            var root = Doc.ActiveRootRing;
            var ringIds = new List<string>();
            if (root != null)
            {
                ringIds.Add(root.Id);
                ringIds.AddRange(Doc.Rings.Where(r => r.IsChildRing && r.ParentRingId == root.Id).Select(r => r.Id));
            }
            foreach (var id in ringIds)
            {
                var ring = Doc.FindRing(id);
                if (ring == null) continue;
                var item = new ComboBoxItem { Content = ring.IsChildRing ? $"{ring.Name} (child ring)" : ring.Name, Tag = ring.Id };
                RingCombo.Items.Add(item);
                if (ring.Id == _selectedRingId) RingCombo.SelectedItem = item;
            }
            if (RingCombo.SelectedItem == null && RingCombo.Items.Count > 0)
            {
                RingCombo.SelectedIndex = 0;
            }
            BindEditor();
        }

        private void BindEditor()
        {
            var ring = SelectedRing ?? Doc.ActiveRootRing;
            if (ring == null)
            {
                RingEditor.Bind(Doc, new RadialRing { Id = "", Name = "(none)", SectorCount = 6 }, -1);
                return;
            }
            _selectedRingId = ring.Id;
            RingEditor.Bind(Doc, ring, _selectedSector);
            RebuildSectorCombo(ring);
        }

        private void RebuildSectorCombo(RadialRing ring)
        {
            SectorActionCombo.Items.Clear();
            SectorActionCombo.Items.Add(new ComboBoxItem { Content = "(None)", Tag = "" });
            foreach (var a in Doc.Actions)
            {
                var item = new ComboBoxItem { Content = $"{a.Label} [{a.Type}]", Tag = a.Id };
                SectorActionCombo.Items.Add(item);
            }
            string currentId = (_selectedSector >= 0 && _selectedSector < ring.SectorActionIds.Count)
                ? ring.SectorActionIds[_selectedSector] : "";
            if (_selectedSector < 0)
            {
                SectorActionCombo.SelectedIndex = -1;
                return;
            }
            foreach (ComboBoxItem item in SectorActionCombo.Items)
            {
                if ((string)(item.Tag ?? "") == currentId)
                {
                    SectorActionCombo.SelectedItem = item;
                    break;
                }
            }
        }

        private void RebuildActions()
        {
            var selectedId = (ActionsList.SelectedItem as ActionListItem)?.ActionId;
            ActionsList.Items.Clear();
            foreach (var a in Doc.Actions)
            {
                var item = new ActionListItem(
                    string.IsNullOrWhiteSpace(a.IconGlyph) ? "?" : a.IconGlyph,
                    string.IsNullOrWhiteSpace(a.Label) ? a.Id : a.Label,
                    a.Type,
                    a.Id);
                ActionsList.Items.Add(item);
                if (item.ActionId == selectedId) ActionsList.SelectedItem = item;
            }
            LoadActionEditor();
        }

        // ---------------- Profiles ----------------

        private void OnProfileSelected(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            if (ProfileCombo.SelectedItem is ComboBoxItem item && (string)item.Tag != Doc.ActiveProfileId)
            {
                Doc.ActiveProfileId = (string)item.Tag;
                _selectedRingId = "";
                _selectedSector = -1;
                _loading = true;
                try { RebuildRings(); }
                finally { _loading = false; }
                if (Doc.ActiveProfile is Profile prof)
                {
                    ProfileExclusionsBox.Text = string.Join(Environment.NewLine, prof.Exclusions);
                }
                Commit();
            }
        }

        private async void OnNewProfileClick(object sender, RoutedEventArgs e)
        {
            string? name = await PromptTextAsync("New profile", "Profile name", $"Profile {Doc.Profiles.Count + 1}");
            if (string.IsNullOrWhiteSpace(name)) return;
            string id = UniqueId("profile", Doc.Profiles.Select(p => p.Id));
            string rootRingId = Doc.ActiveRootRing?.Id ?? Doc.Rings.FirstOrDefault(r => !r.IsChildRing)?.Id ?? "";
            Doc.Profiles.Add(new Profile { Id = id, Name = name, RootRingId = rootRingId, Exclusions = new List<string>() });
            Doc.ActiveProfileId = id;
            _selectedRingId = "";
            RebuildProfiles();
            RebuildRings();
            Commit();
        }

        private async void OnRenameProfileClick(object sender, RoutedEventArgs e)
        {
            if (Doc.ActiveProfile is not Profile prof) return;
            string? name = await PromptTextAsync("Rename profile", "Profile name", prof.Name);
            if (string.IsNullOrWhiteSpace(name) || name == prof.Name) return;
            prof.Name = name;
            RebuildProfiles();
            Commit();
        }

        private void OnDuplicateProfileClick(object sender, RoutedEventArgs e)
        {
            if (Doc.ActiveProfile is not Profile prof) return;
            string id = UniqueId("profile", Doc.Profiles.Select(p => p.Id));
            Doc.Profiles.Add(new Profile
            {
                Id = id,
                Name = $"{prof.Name} (copy)",
                RootRingId = prof.RootRingId,
                Exclusions = new List<string>(prof.Exclusions),
            });
            Doc.ActiveProfileId = id;
            RebuildProfiles();
            Commit();
        }

        private void OnDeleteProfileClick(object sender, RoutedEventArgs e)
        {
            if (Doc.ActiveProfile is not Profile prof) return;
            if (Doc.Profiles.Count <= 1)
            {
                Host?.Show(InfoBarSeverity.Warning, "The last profile cannot be deleted.");
                return;
            }
            Doc.Profiles.Remove(prof);
            Doc.ActiveProfileId = Doc.Profiles[0].Id;
            _selectedRingId = "";
            RebuildProfiles();
            RebuildRings();
            Commit();
        }

        private void OnProfileExclusionsChanged(object sender, RoutedEventArgs e)
        {
            if (_loading || Doc.ActiveProfile is not Profile prof) return;
            var lines = ProfileExclusionsBox.Text
                .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(s => s.Trim())
                .Where(s => s.Length > 0)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();
            prof.Exclusions.Clear();
            prof.Exclusions.AddRange(lines);
            Commit();
        }

        // ---------------- Rings ----------------

        private void OnRingSelected(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            if (RingCombo.SelectedItem is ComboBoxItem item)
            {
                _selectedRingId = (string)item.Tag;
                _selectedSector = -1;
                BindEditor();
            }
        }

        private void OnSectorSelected(int sectorIndex)
        {
            _selectedSector = sectorIndex;
            if (SelectedRing is RadialRing ring)
            {
                RebuildSectorCombo(ring);
            }
        }

        private void OnSectorActionSelected(object sender, SelectionChangedEventArgs e)
        {
            if (_loading || _selectedSector < 0) return;
            if (SelectedRing is not RadialRing ring || _selectedSector >= ring.SectorActionIds.Count) return;
            if (SectorActionCombo.SelectedItem is not ComboBoxItem item) return;
            ring.SectorActionIds[_selectedSector] = (string)(item.Tag ?? "");
            RingEditor.Bind(Doc, ring, _selectedSector);
            Commit();
        }

        private void OnAddChildRingClick(object sender, RoutedEventArgs e)
        {
            var root = Doc.ActiveRootRing;
            if (root == null)
            {
                Host?.Show(InfoBarSeverity.Warning, "The active profile has no root ring.");
                return;
            }
            string id = UniqueId("ring_child", Doc.Rings.Select(r => r.Id));
            var child = new RadialRing
            {
                Id = id,
                Name = $"Child Ring {Doc.Rings.Count}",
                SectorCount = 6,
                IsChildRing = true,
                ParentRingId = root.Id,
                SectorActionIds = new List<string>(new string[6]),
            };
            Doc.Rings.Add(child);
            _selectedRingId = id;
            _selectedSector = -1;
            RebuildRings();
            Commit();
        }

        private void OnRemoveChildRingClick(object sender, RoutedEventArgs e)
        {
            if (SelectedRing is not RadialRing ring || !ring.IsChildRing)
            {
                Host?.Show(InfoBarSeverity.Informational, "Select a child ring to remove it.");
                return;
            }
            bool referenced = Doc.Rings.Any(parent =>
                !parent.IsChildRing &&
                parent.SectorActionIds.Any(sectorId =>
                    Doc.FindAction(sectorId) is ActionDefinition a &&
                    a.Type == "OpenChildRing" && a.ChildRingId == ring.Id));
            if (referenced)
            {
                Host?.Show(InfoBarSeverity.Warning, "This child ring is still assigned to a sector. Clear the sector first.");
                return;
            }
            Doc.Rings.Remove(ring);
            foreach (var a in Doc.Actions.Where(a => a.Type == "OpenChildRing" && a.ChildRingId == ring.Id))
            {
                a.ChildRingId = "";
            }
            _selectedRingId = Doc.ActiveRootRing?.Id ?? "";
            _selectedSector = -1;
            RebuildRings();
            Commit();
        }

        // ---------------- Actions ----------------

        private void OnActionSelected(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            LoadActionEditor();
        }

        private ActionDefinition? SelectedAction
        {
            get
            {
                var item = ActionsList.SelectedItem as ActionListItem;
                return item == null ? null : Doc.FindAction(item.ActionId);
            }
        }

        private void LoadActionEditor()
        {
            var action = SelectedAction;
            bool has = action != null;
            ActionLabelBox.IsEnabled = has;
            ActionTypeCombo.IsEnabled = has;
            ActionLabelBox.Text = action?.Label ?? "";
            SelectComboByTag(ActionTypeCombo, action?.Type ?? "None");
            ActionTargetBox.Text = action?.Target ?? "";
            ActionArgumentsBox.Text = action?.Arguments ?? "";
            ActionWorkingDirBox.Text = action?.WorkingDirectory ?? "";
            ActionChordBox.Text = action?.ShortcutChord ?? "";
            SelectComboByTag(WindowsActionCombo, action?.WindowsActionName ?? "Documents");
            IconGlyphBox.Text = action?.IconGlyph ?? "";
            ApprovedCheck.IsChecked = action?.Approved ?? true;
            EnabledCheck.IsChecked = action?.Enabled ?? true;
            RebuildChildRingCombo(action);
            UpdateTypeVisibility(action);
        }

        private void RebuildChildRingCombo(ActionDefinition? action)
        {
            ChildRingCombo.Items.Clear();
            foreach (var ring in Doc.Rings.Where(r => r.IsChildRing))
            {
                var item = new ComboBoxItem { Content = ring.Name, Tag = ring.Id };
                ChildRingCombo.Items.Add(item);
                if (action != null && action.ChildRingId == ring.Id) ChildRingCombo.SelectedItem = item;
            }
        }

        private void UpdateTypeVisibility(ActionDefinition? action)
        {
            string t = action?.Type ?? "None";
            bool targetPath = t is "LaunchApp" or "OpenFile" or "OpenFolder" or "OpenUrl";
            TargetPanel.Visibility = targetPath ? Visibility.Visible : Visibility.Collapsed;
            BrowseFileButton.Visibility = (t is "LaunchApp" or "OpenFile") ? Visibility.Visible : Visibility.Collapsed;
            BrowseFolderButton.Visibility = (t == "OpenFolder") ? Visibility.Visible : Visibility.Collapsed;
            ActionArgumentsBox.Visibility = (t == "LaunchApp") ? Visibility.Visible : Visibility.Collapsed;
            ActionWorkingDirBox.Visibility = (t == "LaunchApp") ? Visibility.Visible : Visibility.Collapsed;
            ChordPanel.Visibility = (t == "SendShortcut") ? Visibility.Visible : Visibility.Collapsed;
            WindowsActionPanel.Visibility = (t == "WindowsAction") ? Visibility.Visible : Visibility.Collapsed;
            ChildRingPanel.Visibility = (t == "OpenChildRing") ? Visibility.Visible : Visibility.Collapsed;
            ApprovedPanel.Visibility = (t is "LaunchApp" or "OpenFile") ? Visibility.Visible : Visibility.Collapsed;
        }

        private void OnActionTypeChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            var action = SelectedAction;
            if (action == null) return;
            if (ActionTypeCombo.SelectedItem is ComboBoxItem item)
            {
                action.Type = (string)item.Tag;
            }
            UpdateTypeVisibility(action);
            RebuildActions();
            Commit();
        }

        private void OnActionFieldChanged(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            var action = SelectedAction;
            if (action == null) return;
            action.Label = ActionLabelBox.Text;
            action.Target = ActionTargetBox.Text;
            action.Arguments = ActionArgumentsBox.Text;
            action.WorkingDirectory = ActionWorkingDirBox.Text;
            action.ShortcutChord = ActionChordBox.Text;
            action.IconGlyph = IconGlyphBox.Text;
            action.Approved = ApprovedCheck.IsChecked == true;
            action.Enabled = EnabledCheck.IsChecked == true;
            if (WindowsActionCombo.SelectedItem is ComboBoxItem wa) action.WindowsActionName = (string)wa.Tag;
            if (ChildRingCombo.SelectedItem is ComboBoxItem cr) action.ChildRingId = (string)cr.Tag;
            RebuildActions();
            RebuildSectorCombo(SelectedRing ?? Doc.ActiveRootRing ?? new RadialRing());
            Commit();
        }

        private void OnAddActionClick(object sender, RoutedEventArgs e)
        {
            string id = UniqueId("act", Doc.Actions.Select(a => a.Id));
            var action = new ActionDefinition { Id = id, Label = "New Action", Type = "None", Approved = true, Enabled = true };
            Doc.Actions.Add(action);
            RebuildActions();
            var item = ActionsList.Items.OfType<ActionListItem>().FirstOrDefault(i => i.ActionId == id);
            if (item != null) ActionsList.SelectedItem = item;
            Commit();
        }

        private void OnDeleteActionClick(object sender, RoutedEventArgs e)
        {
            var action = SelectedAction;
            if (action == null) return;
            Doc.Actions.Remove(action);
            foreach (var ring in Doc.Rings)
            {
                for (int i = 0; i < ring.SectorActionIds.Count; ++i)
                {
                    if (ring.SectorActionIds[i] == action.Id) ring.SectorActionIds[i] = "";
                }
            }
            RebuildActions();
            RebuildRings();
            Commit();
        }

        private void SwapActions(int a, int b)
        {
            if (a < 0 || b < 0 || a >= Doc.Actions.Count || b >= Doc.Actions.Count) return;
            (Doc.Actions[a], Doc.Actions[b]) = (Doc.Actions[b], Doc.Actions[a]);
            RebuildActions();
            Commit();
        }

        private void OnActionUpClick(object sender, RoutedEventArgs e)
        {
            var idx = IndexOfSelectedAction();
            if (idx > 0) SwapActions(idx, idx - 1);
        }

        private void OnActionDownClick(object sender, RoutedEventArgs e)
        {
            var idx = IndexOfSelectedAction();
            if (idx >= 0 && idx + 1 < Doc.Actions.Count) SwapActions(idx, idx + 1);
        }

        private int IndexOfSelectedAction()
        {
            var item = ActionsList.SelectedItem as ActionListItem;
            return item == null ? -1 : Doc.Actions.FindIndex(a => a.Id == item.ActionId);
        }

        // ---------------- Import / Export / Defaults ----------------

        private async void OnImportClick(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            picker.FileTypeFilter.Add(".json");
            InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file == null) return;
            try
            {
                string json = await FileIO.ReadTextAsync(file);
                var imported = JsonSerializer.Deserialize<ConfigDocument>(json, JsonOptions);
                if (imported == null)
                {
                    Host?.Show(InfoBarSeverity.Error, "The selected file is not a valid Keychain configuration.");
                    return;
                }
                Draft.ReplaceImported(imported);
                _selectedRingId = "";
                _selectedSector = -1;
                LoadFromDraft();
                Host?.Show(InfoBarSeverity.Success, "Configuration imported. Imported executable actions require review before they run. Choose Apply to persist.");
            }
            catch (Exception ex)
            {
                Host?.Show(InfoBarSeverity.Error, $"Import failed: {ex.Message}");
            }
        }

        private async void OnExportClick(object sender, RoutedEventArgs e)
        {
            var picker = new FileSavePicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary, SuggestedFileName = "keychain-config" };
            picker.FileTypeChoices.Add("Keychain configuration", new List<string> { ".json" });
            InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSaveFileAsync();
            if (file == null) return;
            try
            {
                string json = JsonSerializer.Serialize(Draft.Current, JsonOptions);
                await FileIO.WriteTextAsync(file, json);
                Host?.Show(InfoBarSeverity.Success, "Configuration exported.");
            }
            catch (Exception ex)
            {
                Host?.Show(InfoBarSeverity.Error, $"Export failed: {ex.Message}");
            }
        }

        private void OnRestoreDefaultsClick(object sender, RoutedEventArgs e)
        {
            var defaults = DefaultConfigFactory.Create();
            Draft.ReplaceImported(defaults);
            _selectedRingId = "";
            _selectedSector = -1;
            LoadFromDraft();
            Host?.Show(InfoBarSeverity.Informational, "Default configuration restored in the draft. Choose Apply to persist.");
        }

        private async void OnBrowseTargetClick(object sender, RoutedEventArgs e)
        {
            var action = SelectedAction;
            if (action == null) return;
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            picker.FileTypeFilter.Add("*");
            InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file == null) return;
            action.Target = file.Path;
            ActionTargetBox.Text = file.Path;
            Commit();
        }

        private async void OnBrowseFolderClick(object sender, RoutedEventArgs e)
        {
            var action = SelectedAction;
            if (action == null) return;
            var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var folder = await picker.PickSingleFolderAsync();
            if (folder == null) return;
            action.Target = folder.Path;
            ActionTargetBox.Text = folder.Path;
            Commit();
        }

        // ---------------- Helpers ----------------

        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            WriteIndented = true,
            PropertyNameCaseInsensitive = true,
        };

        private static string UniqueId(string prefix, IEnumerable<string> existing)
        {
            var set = new HashSet<string>(existing, StringComparer.Ordinal);
            int n = 1;
            while (set.Contains($"{prefix}_{n}")) ++n;
            return $"{prefix}_{n}";
        }

        private static void SelectComboByTag(ComboBox combo, string tag)
        {
            foreach (ComboBoxItem item in combo.Items)
            {
                if ((string)(item.Tag ?? "") == tag)
                {
                    combo.SelectedItem = item;
                    return;
                }
            }
            if (combo.Items.Count > 0) combo.SelectedIndex = 0;
        }

        private async Task<string?> PromptTextAsync(string title, string header, string initial)
        {
            var box = new TextBox { Header = header, Text = initial };
            var dialog = new ContentDialog
            {
                Title = title,
                Content = box,
                PrimaryButtonText = "OK",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Primary,
                XamlRoot = XamlRoot,
            };
            return await dialog.ShowAsync() == ContentDialogResult.Primary ? box.Text : null;
        }
    }
}
