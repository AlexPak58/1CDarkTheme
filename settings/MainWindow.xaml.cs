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

    private readonly ColorValue[] _colors = new ColorValue[Palette.ColorCount];
    private readonly Dictionary<int, CheckBox> _checks = [];
    private readonly Dictionary<int, TextBox> _visibleInputs = [];
    private PresetStore? _store;
    private List<Preset> _custom = [];
    private int _mask;
    private bool _applying;

    public MainWindow()
    {
        InitializeComponent();
        PlatformPath.Text = @"C:\Program Files\1cv8\8.3.27.2130\bin\1cv8.exe";
        string? storeError = null;
        try
        {
            _store = new PresetStore();
            _custom = _store.Load();
        }
        catch (Exception error) when (IsFileError(error) || error is ArgumentException)
        {
            _store = null;
            storeError = "Пресеты не загружены. Изменение файла отключено: " + error.Message;
        }

        foreach (var zone in Palette.Zones)
        {
            var check = new CheckBox { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(2, 0, 6, 0) };
            AutomationProperties.SetName(check, "Применять тему: " + zone.Label);
            var line = new DockPanel { Margin = new Thickness(0, 3, 0, 3) };
            line.Children.Add(check);
            line.Children.Add(new TextBlock { Text = zone.Label, TextWrapping = TextWrapping.Wrap,
                                              VerticalAlignment = VerticalAlignment.Center });
            var item = new ListBoxItem { Content = line, Tag = zone, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            AutomationProperties.SetName(item, zone.Label);
            ZoneList.Items.Add(item);
            check.Checked += (_, _) => SetZone(item, zone.Bit, true);
            check.Unchecked += (_, _) => SetZone(item, zone.Bit, false);
            _checks.Add(zone.Bit, check);
        }

        SaveButton.IsEnabled = _store is not null;
        RefreshPresets(Palette.DefaultName);
        ZoneList.SelectedIndex = 0;
        if (storeError is not null)
            ShowStatus(storeError);
    }

    private void RefreshPresets(string select)
    {
        var customNames = _custom.Select(p => p.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var builtIns = Palette.BuiltIns.Select(p =>
        {
            var name = p.Name;
            for (var suffix = 1; customNames.Contains(name); suffix++)
                name = $"{p.Name} ({suffix})";
            return p with { Name = name };
        });
        PresetBox.ItemsSource = builtIns.Concat(_custom).ToArray();
        PresetBox.SelectedItem = ((Preset[])PresetBox.ItemsSource).First(p => p.Name == select);
    }

    private void PresetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (PresetBox.SelectedItem is not Preset preset)
            return;
        _applying = true;
        try
        {
            _mask = preset.Mask;
            for (var i = 0; i < Palette.ColorCount; i++)
                _colors[i] = new ColorValue(preset.Colors[i], preset.Colors[i]);
            foreach (var (bit, check) in _checks)
                check.IsChecked = (_mask & bit) != 0;
        }
        finally
        {
            _applying = false;
        }
        DeleteButton.IsEnabled = _store is not null && _custom.Contains(preset);
        DisplaySelectedZone();
        ShowStatus(_mask == 0 ? "Все области выключены. Будут использованы штатные цвета 1С." : "Пресет: " + preset.Name);
    }

    private void ZoneChanged(object sender, SelectionChangedEventArgs e) => DisplaySelectedZone();

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

    private void ShowStatus(string message) => Status.Text = message;

    private void ShowChangeStatus()
    {
        if (_colors.Any(value => value.Normalized is null))
            ShowStatus("Исправьте цвет в формате #RRGGBB перед сохранением или запуском.");
        else if (_mask == 0)
            ShowStatus("Все области выключены. Будут использованы штатные цвета 1С.");
        else
            ShowStatus("Палитра изменена. Можно запустить её или сохранить как пресет.");
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

    private void SavePreset(object sender, RoutedEventArgs e)
    {
        if (_store is null || GetValidColors() is not { } colors)
            return;
        var current = PresetBox.SelectedItem as Preset;
        var name = AskPresetName(current is not null && _custom.Contains(current) ? current.Name : "");
        if (name is null)
            return;
        var existing = _custom.FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
        if (existing is not null && MessageBox.Show(this, $"Заменить пресет «{existing.Name}»?", "Сохранение пресета",
            MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        var next = new List<Preset>(_custom);
        if (existing is not null)
            next.Remove(existing);
        next.Add(new Preset(name, _mask, colors));
        if (Persist(next))
        {
            RefreshPresets(name);
            ShowStatus($"Пресет «{name}» сохранён.");
        }
    }

    private void DeletePreset(object sender, RoutedEventArgs e)
    {
        if (_store is null || PresetBox.SelectedItem is not Preset preset || !_custom.Contains(preset))
            return;
        if (MessageBox.Show(this, $"Удалить пресет «{preset.Name}»?", "Удаление пресета",
            MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        if (Persist(_custom.Where(other => other != preset).ToList()))
        {
            RefreshPresets(Palette.DefaultName);
            ShowStatus($"Пресет «{preset.Name}» удалён.");
        }
    }

    private bool Persist(List<Preset> next)
    {
        try
        {
            _store!.Save(next);
            _custom = next;
            return true;
        }
        catch (Exception error) when (IsFileError(error))
        {
            ShowStatus("Не удалось сохранить изменения пресетов: " + error.Message);
            return false;
        }
    }

    private string? AskPresetName(string initialName)
    {
        var content = new StackPanel { Margin = new Thickness(17) };
        var input = new TextBox { Text = initialName, MaxLength = Palette.MaxPresetName };
        var label = new Label { Content = "Имя пресета", Target = input };
        AutomationProperties.SetName(input, "Имя пресета");
        var message = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 5, 0, 0) };
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
                                       Margin = new Thickness(0, 12, 0, 0) };
        var save = new Button { Content = "Сохранить", IsDefault = true };
        var cancel = new Button { Content = "Отмена", IsCancel = true, Margin = new Thickness(8, 0, 0, 0) };
        actions.Children.Add(save);
        actions.Children.Add(cancel);
        content.Children.Add(label);
        content.Children.Add(input);
        content.Children.Add(message);
        content.Children.Add(actions);
        var dialog = new Window { Owner = this, Title = "Сохранить пресет", Content = content, Width = 385,
                                  SizeToContent = SizeToContent.Height, ResizeMode = ResizeMode.NoResize,
                                  ShowInTaskbar = false, WindowStartupLocation = WindowStartupLocation.CenterOwner };
        dialog.Loaded += (_, _) => { input.Focus(); input.SelectAll(); };
        save.Click += (_, _) =>
        {
            var name = input.Text.Trim();
            if (!Palette.IsValidName(name))
                message.Text = "Введите имя пресета.";
            else if (Palette.BuiltIns.Any(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
                     && !_custom.Any(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)))
                message.Text = "Это имя встроенного пресета. Выберите другое.";
            else
                dialog.DialogResult = true;
        };
        return dialog.ShowDialog() == true ? input.Text.Trim() : null;
    }

    private void BrowsePlatform(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "Выберите 1cv8.exe", Filter = "Платформа 1С (1cv8.exe)|1cv8.exe",
                                          CheckFileExists = true, FileName = "1cv8.exe" };
        if (dialog.ShowDialog(this) == true)
            PlatformPath.Text = dialog.FileName;
    }

    private async void LaunchConfigurator(object sender, RoutedEventArgs e)
    {
        // Capture a complete palette before starting the process. Later UI edits only affect future launches.
        if (GetValidColors() is not { } colors)
            return;
        var platform = PlatformPath.Text.Trim().Trim('"');
        if (!Path.IsPathFullyQualified(platform) || !File.Exists(platform) ||
            !string.Equals(Path.GetFileName(platform), "1cv8.exe", StringComparison.OrdinalIgnoreCase))
        {
            ShowStatus("Выберите файл 1cv8.exe; нужен полный путь.");
            PlatformPath.Focus();
            return;
        }
        var target = DatabaseTarget.Text.Trim();
        var separator = target.IndexOf('\\');
        var fileBase = Path.IsPathFullyQualified(target) && File.Exists(Path.Combine(target, "1Cv8.1CD"));
        if (!fileBase && (separator <= 0 || separator == target.Length - 1 || target.LastIndexOf('\\') != separator
            || target.Any(char.IsControl) || target.IndexOfAny(['"', ';', '/']) >= 0
            || string.IsNullOrWhiteSpace(target[..separator]) || string.IsNullOrWhiteSpace(target[(separator + 1)..])))
        {
            ShowStatus("Укажите сервер\\база или полный путь к каталогу файловой базы.");
            DatabaseTarget.Focus();
            return;
        }
        var launcher = Path.Combine(AppContext.BaseDirectory, "tree-theme-run.exe");
        if (!File.Exists(launcher))
        {
            ShowStatus("Рядом с окном настроек не найден tree-theme-run.exe.");
            return;
        }

        var mask = _mask;
        var start = new ProcessStartInfo(launcher)
        {
            WorkingDirectory = AppContext.BaseDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add(platform);
        start.ArgumentList.Add(target);
        start.ArgumentList.Add("--palette");
        start.ArgumentList.Add(Palette.Encode(mask, colors));

        LaunchButton.IsEnabled = false;
        ShowStatus("Запускается новый Конфигуратор. Если появится окно входа, войдите в базу.");
        try
        {
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows не создала процесс лаунчера.");
            var stdout = process.StandardOutput.ReadToEndAsync();
            var stderr = process.StandardError.ReadToEndAsync();
            await Task.WhenAll(stdout, stderr, process.WaitForExitAsync());
            if (process.ExitCode == 0)
                ShowStatus(mask == 0 ? "Конфигуратор запущен со штатными цветами." : "Конфигуратор запущен, палитра применена.");
            else
            {
                var detail = (stderr.Result.Length > 0 ? stderr.Result : stdout.Result).Trim();
                detail = string.Join(' ', detail.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries));
                detail = detail.Length > 270 ? detail[..270] + "…" : detail;
                ShowStatus($"Ошибка лаунчера (код {process.ExitCode}). {detail}".TrimEnd());
            }
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            ShowStatus("Не удалось запустить или дождаться лаунчера: " + error.Message);
        }
        finally
        {
            LaunchButton.IsEnabled = true;
        }
    }

    private static bool IsFileError(Exception error) =>
        error is IOException or InvalidDataException or UnauthorizedAccessException or SecurityException or JsonException;
}
