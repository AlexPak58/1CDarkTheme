using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using Microsoft.Win32;
using Forms = System.Windows.Forms;

namespace ConfiguratorThemeSettings;

public partial class App : Application
{
    private const string StartupValue = "1CDarkTheme";
    private const string StartupKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private Mutex? _instance;
    private bool _ownsMutex;
    private EventWaitHandle? _showEvent;
    private RegisteredWaitHandle? _showRegistration;
    private Forms.NotifyIcon? _tray;
    private Forms.ToolStripMenuItem? _toggle;
    private ProcessMonitor? _monitor;
    private MainWindow? _window;
    private bool _exitPending;
    private string _configurationPath = "";
    internal bool IsExiting { get; private set; }

    protected override void OnStartup(StartupEventArgs args)
    {
        base.OnStartup(args);
        try
        {
            var hidden = args.Args.Contains("--tray");
            var observeOnly = args.Args.Contains("--observe-only");
            int? onlyPid = null;
            for (var i = 0; i < args.Args.Length; i++)
            {
                if (args.Args[i] is "--tray" or "--observe-only") continue;
                if (args.Args[i] == "--only-pid" && i + 1 < args.Args.Length &&
                    int.TryParse(args.Args[++i], out var pid) && pid > 0) onlyPid = pid;
                else throw new ArgumentException("Неизвестный аргумент запуска утилиты.");
            }

            // One agent per interactive session, including copies from other package paths.
            _instance = new Mutex(true, @"Local\1CDarkTheme.Agent", out _ownsMutex);
            _showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\1CDarkTheme.ShowSettings");
            if (!_ownsMutex) { _showEvent.Set(); Shutdown(); return; }
            _showRegistration = ThreadPool.RegisterWaitForSingleObject(_showEvent,
                (_, _) => Dispatcher.BeginInvoke(ShowSettings), null, Timeout.Infinite, false);
            _configurationPath = ThemeConfiguration.DefaultPath;
            BootstrapFiles(_configurationPath);
            _window = new MainWindow(_configurationPath);
            if (observeOnly || onlyPid is not null)
                _window.Title += observeOnly ? " — только наблюдение" : $" — проверка PID {onlyPid}";
            MainWindow = _window;

            var menu = new Forms.ContextMenuStrip();
            menu.Items.Add("Настройки…", null, (_, _) => ShowSettings());
            _toggle = new Forms.ToolStripMenuItem("Приостановить тему", null, (_, _) => ToggleEnabled());
            menu.Items.Add(_toggle);
            menu.Items.Add(new Forms.ToolStripSeparator());
            menu.Items.Add("Выход и вернуть штатные цвета", null, async (_, _) => await ExitAgentAsync());
            _tray = new Forms.NotifyIcon { Icon = System.Drawing.SystemIcons.Application,
                Text = "Тема 1С", ContextMenuStrip = menu, Visible = true };
            _tray.DoubleClick += (_, _) => ShowSettings();
            _monitor = new ProcessMonitor(_configurationPath, Path.Combine(AppContext.BaseDirectory, "tree-theme-run.exe"), observeOnly, onlyPid);
            _monitor.SnapshotChanged += snapshot => Dispatcher.BeginInvoke(() =>
            {
                _window.SetMonitorSnapshot(snapshot);
                if (_toggle is not null) _toggle.Text = snapshot.Enabled == true ? "Приостановить тему" : "Включить тему";
            });
            _monitor.Start();
            if (!hidden) ShowSettings();
        }
        catch (Exception error)
        {
            MessageBox.Show("Не удалось запустить утилиту: " + error.Message, "Тема 1С", MessageBoxButton.OK, MessageBoxImage.Error);
            IsExiting = true;
            Shutdown(1);
        }
    }

    private static void BootstrapFiles(string configPath)
    {
        var directory = Path.GetDirectoryName(configPath)!;
        var presets = Path.Combine(directory, "presets");
        Directory.CreateDirectory(presets);
        foreach (var name in new[] { "dark.json", "graphite.json", "pink.json", "native.json" })
        {
            var destination = Path.Combine(presets, name);
            if (!File.Exists(destination))
                File.Copy(Path.Combine(AppContext.BaseDirectory, "presets", name), destination, overwrite: false);
        }
        if (!File.Exists(configPath))
            ThemeConfiguration.Save(configPath, ThemeConfiguration.CreateDefault(configPath), "");
    }

    private void ShowSettings()
    {
        if (_window is null || IsExiting) return;
        _window.RefreshConfiguration();
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    private void ToggleEnabled()
    {
        try
        {
            var loaded = ThemeConfiguration.Load(_configurationPath);
            ThemeConfiguration.Save(_configurationPath, loaded.Configuration with { Enabled = !loaded.Configuration.Enabled }, loaded.Hash);
            _window?.RefreshConfiguration();
        }
        catch (Exception error) { MessageBox.Show(error.Message, "Настройки темы", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    internal async Task ExitAgentAsync()
    {
        if (_exitPending || _window?.ConfirmExit() == false) return;
        _exitPending = true;
        try
        {
            if (_monitor is not null) await _monitor.StopAsync(restoreOwned: true);
        }
        catch (Exception error)
        {
            ShowSettings();
            if (MessageBox.Show("Не удалось полностью вернуть штатные цвета: " + error.Message +
                "\nФоновое применение остановлено. Завершить утилиту? Для полного снятия темы можно закрыть соответствующий сеанс 1С.",
                "Завершение темы 1С", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            { _exitPending = false; return; }
        }
        IsExiting = true;
        Shutdown();
    }

    private static string StartupCommand => "\"" + (Environment.ProcessPath ?? throw new InvalidOperationException("Не определён путь приложения.")) + "\" --tray";
    internal static bool StartupEnabled
    {
        get { using var key = Registry.CurrentUser.OpenSubKey(StartupKey); return string.Equals(key?.GetValue(StartupValue) as string, StartupCommand, StringComparison.OrdinalIgnoreCase); }
    }
    internal static void SetStartup(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(StartupKey);
        if (enabled) key.SetValue(StartupValue, StartupCommand, RegistryValueKind.String);
        else if (string.Equals(key.GetValue(StartupValue) as string, StartupCommand, StringComparison.OrdinalIgnoreCase)) key.DeleteValue(StartupValue, false);
    }

    protected override void OnExit(ExitEventArgs args)
    {
        _tray?.Dispose();
        _showRegistration?.Unregister(null);
        _showEvent?.Dispose();
        if (_ownsMutex) _instance?.ReleaseMutex();
        _instance?.Dispose();
        base.OnExit(args);
    }
}
