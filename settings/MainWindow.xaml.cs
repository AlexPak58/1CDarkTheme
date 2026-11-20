using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;
using Forms = System.Windows.Forms;

namespace ConfiguratorThemeSettings;

public partial class MainWindow : Window
{
    private readonly record struct ColorValue(string? Normalized, string Input);
    private sealed record WindowOwner(IntPtr Handle) : Forms.IWin32Window;
    private sealed record ThemeChoice(string Name, string Path, Preset Preset);
    private readonly ColorValue[] _colors = new ColorValue[Palette.ColorCount];
    private readonly Dictionary<int, CheckBox> _checks = [];
    private readonly Dictionary<int, TextBox> _visibleInputs = [];
    private readonly string _configurationPath;
    private string? _loadedPresetPath;
    private string? _loadedPresetHash;
    private int _mask;
    private bool _applying = true;
    private bool _dirty;
    private bool _globalDraft;
    internal EnterpriseThemeWindow? EnterpriseTheme { get; set; }

    public MainWindow() : this(ThemeConfiguration.DefaultPath) { }

    internal MainWindow(string configurationPath)
    {
        _configurationPath = configurationPath;
        InitializeComponent();
        PlatformPath.Text = @"C:\Program Files\1cv8\8.3.27.2130\bin\1cv8.exe";
        foreach (var zone in Palette.Zones)
        {
            var check = new CheckBox { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(2, 0, 6, 0) };
            AutomationProperties.SetName(check, "Применять тему: " + zone.Label);
            var line = new DockPanel { Margin = new Thickness(0, 5, 0, 5) };
            line.Children.Add(check);
            line.Children.Add(new TextBlock { Text = zone.Label, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
            var item = new ListBoxItem { Content = line, Tag = zone, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            AutomationProperties.SetName(item, zone.Label);
            ZoneList.Items.Add(item);
            check.Checked += (_, _) => SetZone(item, zone.Bit, true);
            check.Unchecked += (_, _) => SetZone(item, zone.Bit, false);
            _checks.Add(zone.Bit, check);
        }
        PresetName.TextChanged += (_, _) => { if (!_applying) ShowChangeStatus(); };
        GlobalPresetPath.TextChanged += (_, _) => { if (!_applying) _globalDraft = true; };
        RefreshConfiguration();
        try
        {
            var config = ThemeConfiguration.Load(_configurationPath).Configuration;
            ReadPreset(Path.GetFullPath(config.DefaultPresetFile, Path.GetDirectoryName(_configurationPath)!));
        }
        catch (Exception error) when (IsFileError(error)) { ShowStatus(error.Message); }
        _applying = false;
        Closing += (_, args) =>
        {
            if (Application.Current is App { IsExiting: false })
            {
                args.Cancel = true;
                EnterpriseTheme?.ClearTarget();
                Hide();
            }
        };
    }

    internal bool ConfirmExit() => !_dirty || MessageBox.Show(this,
        "В редакторе есть несохранённые изменения. Выйти без их сохранения?", "Тема 1С",
        MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) == MessageBoxResult.Yes;

    private async void ExitApplication(object sender, RoutedEventArgs args)
    {
        if (Application.Current is App app) await app.ExitAgentAsync();
    }

    internal void RefreshConfiguration()
    {
        var prior = _applying;
        _applying = true;
        try
        {
            var config = ThemeConfiguration.Load(_configurationPath).Configuration;
            EnabledBox.IsChecked = config.Enabled;
            if (!_globalDraft) GlobalPresetPath.Text = config.DefaultPresetFile;
            RefreshThemeChoices(config.DefaultPresetFile);
            RulesGrid.ItemsSource = config.Rules;
            StartupBox.IsChecked = App.StartupEnabled;
        }
        catch (Exception error) when (IsFileError(error)) { ShowStatus("Настройки не прочитаны: " + error.Message); }
        finally { _applying = prior; }
    }

    internal void SelectTheme() => MainTabs.SelectedItem = ThemeTab;

    private void RefreshThemeChoices(string defaultFile)
    {
        var currentPath = ResolveFile(defaultFile);
        var selectedPath = (ThemeChoices.SelectedItem as ThemeChoice)?.Path ?? currentPath;
        if (!_globalDraft) selectedPath = currentPath;
        var directory = Path.Combine(Path.GetDirectoryName(_configurationPath)!, "presets");
        IEnumerable<string> files = Directory.Exists(directory) ? Directory.EnumerateFiles(directory, "*.json") : [];
        var choices = new List<ThemeChoice>();
        foreach (var file in files.Append(currentPath).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                var preset = PresetFile.Load(file).Preset;
                choices.Add(new ThemeChoice(preset.Name,file,preset));
            }
            catch (Exception error) when (IsFileError(error)) { }
        }
        ThemeChoices.ItemsSource = choices.OrderBy(choice => choice.Name, StringComparer.CurrentCultureIgnoreCase).ToArray();
        if (EnterpriseTheme is { IsDesignerTarget: true } target)
        {
            var profile = target.WindowPreset;
            var applied = profile is null ? null : choices.Find(choice => choice.Preset.Mask == profile.Mask &&
                (profile.Mask == 0 || choice.Preset.Colors.SequenceEqual(profile.Colors,StringComparer.OrdinalIgnoreCase)));
            ThemeChoices.SelectedItem = applied;
            CurrentTheme.Text = applied is null ? "Конфигуратор" : "Текущая тема — " + applied.Name;
            ThemeScope.Text = "Пресет применяется к текущему окну Конфигуратора.";
        }
        else
        {
            ThemeChoices.SelectedItem = choices.Find(choice => string.Equals(choice.Path,selectedPath,StringComparison.OrdinalIgnoreCase));
            var current = choices.Find(choice => string.Equals(choice.Path,currentPath,StringComparison.OrdinalIgnoreCase));
            CurrentTheme.Text = "Текущая тема — " + (current?.Name ?? Path.GetFileName(currentPath));
            ThemeScope.Text = "В окне Предприятия выбор сразу применяется к этому окну.";
        }
        ApplyThemeButton.IsEnabled = ThemeChoices.SelectedItem is ThemeChoice;
    }

    private void ThemeChoiceChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_applying) return;
        ApplyThemeButton.IsEnabled = ThemeChoices.SelectedItem is ThemeChoice;
        if (EnterpriseTheme?.IsDesignerTarget != true && ThemeChoices.SelectedItem is ThemeChoice choice)
            GlobalPresetPath.Text = StorePath(choice.Path);
    }

    private void ApplyThemeChoice(object sender, RoutedEventArgs args)
    {
        if (ThemeChoices.SelectedItem is ThemeChoice choice)
        {
            if (EnterpriseTheme is { IsDesignerTarget: true } target)
            {
                Guard(() =>
                {
                    var preset = PresetFile.Load(choice.Path).Preset;
                    if (target.Apply(preset) == true)
                    {
                        CurrentTheme.Text = "Текущая тема — " + preset.Name;
                        ShowStatus("Пресет применён к текущему окну Конфигуратора.");
                    }
                    else ShowStatus("Окно Конфигуратора не подтвердило применение пресета.");
                });
                return;
            }
            GlobalPresetPath.Text = StorePath(choice.Path);
            ApplyGlobal(sender,args);
        }
    }

    internal void SetMonitorSnapshot(MonitorSnapshot snapshot)
    {
        ProcessesGrid.ItemsSource = snapshot.Processes.Select(item => item with { Status = StatusLabel(item.Status) }).ToArray();
        MonitorStatus.Text = snapshot.ConfigurationError ?? $"Найдено сеансов: {snapshot.Processes.Count}. " +
            (snapshot.Enabled == true ? "Фоновое применение включено." : "Фоновое применение выключено.");
    }

    private static string StatusLabel(string status) => status switch
    {
        "applied" => "Тема применена", "disabled" => "Штатные цвета", "not_ready" => "Ожидание загрузки",
        "unsupported" => "Пока не поддерживается", "conflict" => "Другая версия DLL", "ambiguous" => "Требуется перезапуск 1С",
        "config_error" => "Ошибка настроек", "preset_error" => "Ошибка файла темы", "observed" => "Только наблюдение",
        "access_denied" => "Нет доступа", "stale" => "Сеанс завершён", _ => status
    };

    private string ResolveFile(string input) => Path.GetFullPath(input.Trim().Trim('"'), Path.GetDirectoryName(_configurationPath)!);
    private string StorePath(string path) => Path.GetRelativePath(Path.GetDirectoryName(_configurationPath)!, path);
    private void ChangeConfiguration(Func<ThemeConfiguration, ThemeConfiguration> change)
    {
        var loaded = ThemeConfiguration.Load(_configurationPath);
        ThemeConfiguration.Save(_configurationPath, change(loaded.Configuration), loaded.Hash);
        RefreshConfiguration();
        ShowStatus("Настройки сохранены. Фоновый агент применит их к открытым сеансам.");
    }
    private void Guard(Action action)
    {
        try { action(); }
        catch (Exception error) when (IsFileError(error) || error is Win32Exception || error is InvalidOperationException)
        { ShowStatus(error.Message); }
    }
    private void EnabledChanged(object sender, RoutedEventArgs args)
    {
        if (!_applying) Guard(() => ChangeConfiguration(config => config with { Enabled = EnabledBox.IsChecked == true }));
    }
    private void StartupChanged(object sender, RoutedEventArgs args)
    {
        if (!_applying) Guard(() => { App.SetStartup(StartupBox.IsChecked == true); ShowStatus("Настройка запуска Windows сохранена."); });
    }
    private string? ChoosePresetFile()
    {
        var dialog = new OpenFileDialog { Title = "Выбрать файл пресета", Filter = "Пресеты JSON|*.json", CheckFileExists = true,
            InitialDirectory = Path.Combine(Path.GetDirectoryName(_configurationPath)!, "presets") };
        return dialog.ShowDialog(this) == true ? dialog.FileName : null;
    }
    private void BrowseGlobal(object sender, RoutedEventArgs args) { if (ChoosePresetFile() is { } path) GlobalPresetPath.Text = StorePath(path); }
    private void BrowseRule(object sender, RoutedEventArgs args) { if (ChoosePresetFile() is { } path) RulePresetPath.Text = StorePath(path); }
    private void ApplyGlobal(object sender, RoutedEventArgs args) => Guard(() =>
    {
        var path = ResolveFile(GlobalPresetPath.Text);
        var preset = PresetFile.Load(path).Preset;
        ChangeConfiguration(config => config with { DefaultPresetFile = StorePath(path) });
        _globalDraft = false;
        RefreshConfiguration();
        ShowStatus(EnterpriseTheme?.Apply(preset) switch
        {
            true => "Пресет применён к окну 1С и сохранён.",
            false => "Пресет сохранён. Открытое окно 1С не подтвердило применение.",
            null => "Пресет сохранён для следующих запусков."
        });
    });
    private static string? Optional(string value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    private static bool SameRule(ThemeRule a, ThemeRule b) =>
        string.Equals(a.BaseId, b.BaseId, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(a.Platform, b.Platform, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(a.Mode, b.Mode, StringComparison.OrdinalIgnoreCase);
    private void SaveRule(object sender, RoutedEventArgs args) => Guard(() =>
    {
        var path = ResolveFile(RulePresetPath.Text);
        _ = PresetFile.Load(path);
        var rule = new ThemeRule(ThemeConfiguration.NormalizeBaseId(Optional(RuleBase.Text)), Optional(RulePlatform.Text),
            (RuleMode.SelectedItem as ComboBoxItem)?.Tag as string, StorePath(path));
        ChangeConfiguration(config => config with { Rules = config.Rules.Where(old => !SameRule(old, rule)).Append(rule).ToArray() });
    });
    private void RemoveRule(object sender, RoutedEventArgs args)
    {
        if (RulesGrid.SelectedItem is ThemeRule selected)
            Guard(() => ChangeConfiguration(config => config with { Rules = config.Rules.Where(rule => !SameRule(rule, selected)).ToArray() }));
        else ShowStatus("Выберите исключение в таблице.");
    }
    private void ClearRule(object sender, RoutedEventArgs args)
    { RulesGrid.SelectedItem = null; RuleBase.Clear(); RulePlatform.Clear(); RulePresetPath.Clear(); RuleMode.SelectedIndex = 0; }
    private void RuleSelected(object sender, SelectionChangedEventArgs args)
    {
        if (RulesGrid.SelectedItem is not ThemeRule rule) return;
        RuleBase.Text = rule.BaseId ?? ""; RulePlatform.Text = rule.Platform ?? ""; RulePresetPath.Text = rule.PresetFile;
        RuleMode.SelectedIndex = rule.Mode == "Designer" ? 1 : rule.Mode == "Enterprise" ? 2 : 0;
    }
    private bool CanReplaceEditor() => !_dirty || MessageBox.Show(this, "Отбросить несохранённые изменения редактора?", "Файл пресета",
        MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) == MessageBoxResult.Yes;
    private void OpenPreset(object sender, RoutedEventArgs args)
    {
        if (CanReplaceEditor() && ChoosePresetFile() is { } path) Guard(() => ReadPreset(path));
    }
    private void LoadPresetPath(object sender, RoutedEventArgs args)
    { if (CanReplaceEditor()) Guard(() => ReadPreset(ResolveFile(PresetPath.Text))); }
    private void ReadPreset(string path)
    {
        var loaded = PresetFile.Load(path);
        _applying = true;
        try
        {
            _loadedPresetPath = Path.GetFullPath(path); _loadedPresetHash = loaded.Hash;
            PresetPath.Text = _loadedPresetPath; PresetName.Text = loaded.Preset.Name; _mask = loaded.Preset.Mask;
            for (var i = 0; i < _colors.Length; i++) _colors[i] = new ColorValue(loaded.Preset.Colors[i], loaded.Preset.Colors[i]);
            foreach (var (bit, check) in _checks) check.IsChecked = (_mask & bit) != 0;
            if (ZoneList.SelectedIndex < 0) ZoneList.SelectedIndex = 0;
            DisplaySelectedZone(); _dirty = false;
            ShowStatus("Загружен файл: " + _loadedPresetPath);
        }
        finally { _applying = false; }
    }
    private void ZoneChanged(object sender, SelectionChangedEventArgs args) => DisplaySelectedZone();
    private void SetZone(ListBoxItem item, int bit, bool enabled)
    {
        if (_applying)
            return;
        _mask = enabled ? _mask | bit : _mask & ~bit;
        ZoneList.SelectedItem = item;
        ShowChangeStatus();
    }

    private void DisplaySelectedZone()
    {
        if (ZoneList.SelectedItem is not ListBoxItem { Tag: Zone zone })
            return;

        ZoneTitle.Text = zone.Label;
        ColorRows.Children.Clear();
        _visibleInputs.Clear();
        if (zone.Hint is { } zoneHint)
            ColorRows.Children.Add(new TextBlock { Text = zoneHint, TextWrapping = TextWrapping.Wrap,
                                                   Margin = new Thickness(0, 0, 0, 10) });
        for (var i = 0; i < zone.Fields.Length; i++)
        {
            var colorIndex = zone.Offset + i;
            var fieldName = zone.Fields[i];
            var label = new TextBlock { Text = fieldName, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
            var input = new TextBox { Text = _colors[colorIndex].Input, Margin = new Thickness(8, 0, 0, 0),
                                      FontFamily = new FontFamily("Consolas") };
            AutomationProperties.SetName(input, fieldName + " — " + zone.Label);
            _visibleInputs.Add(colorIndex, input);
            var sample = new Border { Width = 28, Height = 16, CornerRadius = new CornerRadius(2) };
            var choose = new Button { Content = sample, Padding = new Thickness(4), Margin = new Thickness(8, 0, 0, 0),
                                      ToolTip = "Выбрать цвет: " + fieldName };
            AutomationProperties.SetName(choose, "Выбрать цвет: " + fieldName + " — " + zone.Label);
            var hint = new TextBlock { Text = "Нужно 6 шестнадцатеричных цифр, например #1F2938.",
                                       TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 3, 0, 0) };
            var row = new Grid { Margin = new Thickness(0, 0, 0, 10) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(94) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.RowDefinitions.Add(new RowDefinition());
            row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetColumn(input, 1);
            Grid.SetColumn(choose, 2);
            Grid.SetRow(hint, 1);
            Grid.SetColumnSpan(hint, 3);
            row.Children.Add(label);
            row.Children.Add(input);
            row.Children.Add(choose);
            row.Children.Add(hint);
            ColorRows.Children.Add(row);

            void UpdateSwatch()
            {
                var value = _colors[colorIndex].Normalized;
                sample.Background = value is null ? Brushes.Transparent : new SolidColorBrush((Color)ColorConverter.ConvertFromString(value));
                hint.Visibility = value is null ? Visibility.Visible : Visibility.Collapsed;
            }
            UpdateSwatch();

            input.TextChanged += (_, _) =>
            {
                var valid = Palette.TryNormalizeColor(input.Text, out var color);
                _colors[colorIndex] = new ColorValue(valid ? color : null, input.Text);
                UpdateSwatch();
                ShowChangeStatus();
            };
            input.LostFocus += (_, _) =>
            {
                if (_colors[colorIndex].Normalized is string color && input.Text != color)
                    input.Text = color;
            };
            choose.Click += (_, _) =>
            {
                using var picker = new Forms.ColorDialog { FullOpen = true };
                if (_colors[colorIndex].Normalized is string color)
                    picker.Color = System.Drawing.ColorTranslator.FromHtml(color);
                if (picker.ShowDialog(new WindowOwner(new WindowInteropHelper(this).Handle)) == Forms.DialogResult.OK)
                    input.Text = $"#{picker.Color.R:X2}{picker.Color.G:X2}{picker.Color.B:X2}";
            };
        }
    }

    internal void ShowImportWarning(string message) => ShowStatus(message);

    private void ShowStatus(string message) => Status.Text = message;

    private void ShowChangeStatus()
    {
        if (_applying) return;
        _dirty = true;
        if (_colors.Any(value => value.Normalized is null))
            ShowStatus("Исправьте цвет в формате #RRGGBB перед сохранением.");
        else if (_mask == 0)
            ShowStatus("Все области выключены. Будут использованы штатные цвета 1С.");
        else
            ShowStatus("Изменения ещё не сохранены в файл.");
    }

    private string[]? GetValidColors()
    {
        var invalid = Array.FindIndex(_colors, value => value.Normalized is null);
        if (invalid < 0)
            return _colors.Select(value => value.Normalized!).ToArray();
        var zoneIndex = Array.FindLastIndex(Palette.Zones, zone => zone.Offset <= invalid);
        ZoneList.SelectedIndex = zoneIndex;
        ZoneList.ScrollIntoView(ZoneList.SelectedItem);
        _visibleInputs[invalid].Focus();
        ShowStatus("Исправьте цвет в формате #RRGGBB.");
        return null;
    }

    private void SavePreset(object sender, RoutedEventArgs args) => Guard(() =>
    {
        if (_loadedPresetPath is null) { ShowStatus("Сначала откройте файл пресета."); return; }
        if (!string.Equals(ResolveFile(PresetPath.Text), _loadedPresetPath, StringComparison.OrdinalIgnoreCase))
        { ShowStatus("Путь изменён: сначала загрузите файл или используйте «Сохранить как…»."); return; }
        SavePresetTo(_loadedPresetPath, _loadedPresetHash);
    });
    private void SavePresetAs(object sender, RoutedEventArgs args)
    {
        if (GetValidColors() is null) return;
        var dialog = new SaveFileDialog { Title = "Сохранить файл пресета", Filter = "Пресеты JSON|*.json", DefaultExt = ".json",
            AddExtension = true, OverwritePrompt = true, FileName = _loadedPresetPath is null ? "theme.json" : Path.GetFileName(_loadedPresetPath),
            InitialDirectory = _loadedPresetPath is null ? Path.GetDirectoryName(_configurationPath) : Path.GetDirectoryName(_loadedPresetPath) };
        if (dialog.ShowDialog(this) == true) Guard(() => SavePresetTo(dialog.FileName,
            string.Equals(Path.GetFullPath(dialog.FileName), _loadedPresetPath, StringComparison.OrdinalIgnoreCase)
                ? _loadedPresetHash : File.Exists(dialog.FileName) ? PresetFile.Load(dialog.FileName).Hash : ""));
    }
    private void SavePresetTo(string path, string? expectedHash)
    {
        if (GetValidColors() is not { } colors) return;
        var preset = new Preset(PresetName.Text.Trim(), _mask, colors);
        _loadedPresetHash = PresetFile.Save(path, preset, expectedHash);
        _loadedPresetPath = Path.GetFullPath(path); PresetPath.Text = _loadedPresetPath; _dirty = false;
        ShowStatus("Файл сохранён. Сеансы, использующие этот файл, получат изменения автоматически.");
    }
    internal void SelectEnterpriseLaunch()
    {
        MainTabs.SelectedItem = SessionsTab;
        LaunchPanel.IsExpanded = true;
        LaunchMode.SelectedIndex = 2;
    }
    private void LaunchModeChanged(object sender, SelectionChangedEventArgs args)
    {
        if (PlatformPath is null) return;
        PlatformPath.Text = @"C:\Program Files\1cv8\8.3.27.2130\bin\" + (LaunchMode.SelectedIndex == 2 ? "1cv8c.exe" : "1cv8.exe");
    }
    private void BrowsePlatform(object sender, RoutedEventArgs args)
    {
        var dialog = new OpenFileDialog { Title = "Выбрать клиент 1С", Filter = "Клиент 1С|1cv8.exe;1cv8c.exe", CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) PlatformPath.Text = dialog.FileName;
    }
    private async void LaunchConfigurator(object sender, RoutedEventArgs args)
    {
        try
        {
            var enterprise = LaunchMode.SelectedIndex > 0;
            var platform = PlatformPath.Text.Trim().Trim('"');
            var client = Path.GetFileName(platform);
            if (!Path.IsPathFullyQualified(platform) || !File.Exists(platform) ||
                !(client.Equals("1cv8.exe", StringComparison.OrdinalIgnoreCase) || enterprise && client.Equals("1cv8c.exe", StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("Выберите полный путь к клиенту 1С.");
            var target = DatabaseTarget.Text.Trim();
            var fileBase = Path.IsPathFullyQualified(target) && File.Exists(Path.Combine(target, "1Cv8.1CD"));
            var separator = target.IndexOf('\\');
            if (target.Any(char.IsControl) || target.IndexOf('"') >= 0 || (!fileBase &&
                (separator <= 0 || separator == target.Length - 1 || target.LastIndexOf('\\') != separator || target.IndexOfAny([';', '/']) >= 0)))
                throw new InvalidDataException("Укажите сервер\\база или каталог файловой базы.");
            if (!enterprise)
            {
                var designer = new ProcessStartInfo(platform) { UseShellExecute = false };
                designer.ArgumentList.Add("DESIGNER"); designer.ArgumentList.Add(fileBase ? "/F" : "/S"); designer.ArgumentList.Add(target);
                using var process = Process.Start(designer) ?? throw new InvalidOperationException("Не удалось создать процесс 1С.");
                ShowStatus("Конфигуратор запущен. Тему применит фоновый агент по настройкам и исключениям.");
                return;
            }
            var config = ThemeConfiguration.Load(_configurationPath).Configuration;
            var baseId = ThemeConfiguration.NormalizeBaseId((fileBase ? "file:" : "server:") + target);
            var presetPath = config.ResolvePresetFile(baseId, "8.3.27.2130-x64", "Enterprise");
            var preset = config.Enabled ? PresetFile.Load(presetPath).Preset : Palette.BuiltIns[0];
            var start = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "tree-theme-run.exe"))
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            start.ArgumentList.Add(platform); start.ArgumentList.Add(target); start.ArgumentList.Add("--palette");
            start.ArgumentList.Add(Palette.Encode(config.Enabled ? preset.Mask : 0, preset.Colors)); start.ArgumentList.Add("--enterprise");
            LaunchButton.IsEnabled = false;
            ShowStatus("Запускается Предприятие. При необходимости войдите в базу в окне 1С.");
            using var helper = Process.Start(start) ?? throw new InvalidOperationException("Не удалось создать процесс запуска.");
            var output = helper.StandardOutput.ReadToEndAsync(); var errors = helper.StandardError.ReadToEndAsync();
            await Task.WhenAll(output, errors, helper.WaitForExitAsync());
            ShowStatus(helper.ExitCode == 0 ? "Предприятие запущено, палитра применена." : "Применение темы не завершено. " + errors.Result.Trim());
        }
        catch (Exception error) when (IsFileError(error) || error is Win32Exception or InvalidOperationException)
        { ShowStatus(error.Message); }
        finally { LaunchButton.IsEnabled = true; }
    }
    private static bool IsFileError(Exception error) => error is IOException or InvalidDataException or UnauthorizedAccessException or SecurityException or JsonException or ArgumentException or NotSupportedException;
}
