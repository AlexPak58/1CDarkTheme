using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace ConfiguratorThemeSettings;

internal sealed record MonitoredProcess(DetectedProcess Process, string? PresetPath, string? PresetName,
                                        string Status, uint Win32 = 0, uint Dll = 0);

internal sealed record MonitorSnapshot(IReadOnlyList<MonitoredProcess> Processes, string? ConfigurationError,
                                      bool? Enabled);

internal sealed record ApplyResult(string Status, uint Win32 = 0, uint Dll = 0);

internal sealed class ProcessMonitor
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan HelperTimeout = TimeSpan.FromSeconds(65);
    private static readonly string DisabledPalette = Palette.Encode(0, Palette.BuiltIns[0].Colors);
    private const string SupportedPlatform = "8.3.27.2130-x64";
    private const string DisabledProfile = "disabled";

    private readonly string _configurationPath;
    private readonly bool _observeOnly;
    private readonly Func<IReadOnlyList<DetectedProcess>> _scan;
    private readonly Func<ProcessIdentity, string, Task<ApplyResult>> _apply;
    private readonly TimeProvider _clock;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Dictionary<ProcessIdentity, TargetState> _targets = [];
    private Task? _polling;
    private Task? _stopping;
    private MonitorSnapshot? _lastSnapshot;

    private sealed class TargetState
    {
        public string? AttemptedProfile { get; set; }
        public ApplyResult? Result { get; set; }
        public bool OwnsActivePalette { get; set; }
        public int Failures { get; set; }
        public DateTimeOffset RetryAt { get; set; }
    }

    public ProcessMonitor(string configurationPath, string helperPath, bool observeOnly = false, int? processId = null)
        : this(configurationPath, observeOnly, new ProcessDiscovery(), helperPath, processId) { }

    private ProcessMonitor(string configurationPath, bool observeOnly, ProcessDiscovery discovery, string helperPath,
                           int? processId)
        : this(configurationPath, observeOnly, () => discovery.Scan(processId),
               (identity, palette) => RunHelperAsync(helperPath, identity, palette), TimeProvider.System) { }

    // The polling loop and final restoration own all target state in sequence.
    internal ProcessMonitor(string configurationPath, bool observeOnly, Func<IReadOnlyList<DetectedProcess>> scan,
                            Func<ProcessIdentity, string, Task<ApplyResult>> apply, TimeProvider clock)
    {
        _configurationPath = configurationPath;
        _observeOnly = observeOnly;
        _scan = scan;
        _apply = apply;
        _clock = clock;
    }

    public event Action<MonitorSnapshot>? SnapshotChanged;

    public void Start()
    {
        if (_stopping is null)
            _polling ??= Task.Run(PollAsync);
    }

    public Task StopAsync(bool restoreOwned = false) => _stopping ??= StopCoreAsync(restoreOwned);

    private async Task PollAsync()
    {
        var cancellation = _shutdown.Token;
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                await PollOnceAsync(cancellation);
                await Task.Delay(PollInterval, _clock, cancellation);
            }
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
    }

    internal async Task PollOnceAsync(CancellationToken cancellation = default)
    {
        IReadOnlyList<DetectedProcess> detected;
        try
        {
            detected = _scan();
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException)
        {
            Publish(new MonitorSnapshot(_lastSnapshot?.Processes ?? [], "Список процессов 1С временно недоступен.", null));
            return;
        }

        ForgetExitedTargets(detected);

        ThemeConfiguration? config = null;
        string? configurationError = null;
        try
        {
            config = ThemeConfiguration.Load(_configurationPath).Configuration;
        }
        catch (Exception error) when (IsFileError(error))
        {
            configurationError = error is FileNotFoundException or DirectoryNotFoundException
                ? "Файл настроек отсутствует." : "Не удалось прочитать файл настроек.";
        }

        var presets = new Dictionary<string, LoadedPreset?>(StringComparer.OrdinalIgnoreCase);
        var rows = new List<MonitoredProcess>(detected.Count);
        foreach (var process in detected)
        {
            cancellation.ThrowIfCancellationRequested();
            rows.Add(await InspectAsync(process, config, presets));
        }
        Publish(new MonitorSnapshot(rows.AsReadOnly(), configurationError, config?.Enabled));
    }

    private async Task<MonitoredProcess> InspectAsync(DetectedProcess process, ThemeConfiguration? config,
                                                    Dictionary<string, LoadedPreset?> presets)
    {
        string? path = null;
        LoadedPreset? preset = null;
        if (config is not null)
        {
            // The settings schema accepts x86/x64 selectors. Other architectures can still match a base or mode.
            var platform = process.Platform is { } id &&
                (id.EndsWith("-x64", StringComparison.Ordinal) || id.EndsWith("-x86", StringComparison.Ordinal)) ? id : null;
            path = config.ResolvePresetFile(process.BaseId, platform, process.Mode);
            if (!presets.TryGetValue(path, out preset))
            {
                try
                {
                    preset = PresetFile.Load(path);
                }
                catch (Exception error) when (IsFileError(error)) { }
                presets.Add(path, preset);
            }
        }

        MonitoredProcess Row(string status, uint win32 = 0, uint dll = 0) =>
            new(process, path, preset?.Preset.Name, status, win32, dll);

        if (process.DiscoveryStatus is { } discoveryStatus)
            return Row(discoveryStatus);
        if (process.Identity.CreationFileTime == 0 || process.Mode != "Designer" || process.Platform != SupportedPlatform)
            return Row("unsupported");
        if (!_targets.TryGetValue(process.Identity, out var state))
        {
            state = new TargetState();
            _targets.Add(process.Identity, state);
        }
        if (state.Result is { Status: "ambiguous" or "stale" or "conflict" } terminal)
            return Row(terminal.Status, terminal.Win32, terminal.Dll);
        if (config is null)
            return Row("config_error");
        if (config.Enabled && preset is null)
            return Row("preset_error");
        if (_observeOnly)
            return Row("observed");

        var mask = config.Enabled ? preset!.Preset.Mask : 0;
        var profile = config.Enabled ? path + "\0" + preset!.Hash : DisabledProfile;
        var sameProfile = state.AttemptedProfile == profile;
        if (sameProfile && state.Result is { } previous &&
            (previous.Status != "not_ready" || _clock.GetUtcNow() < state.RetryAt))
            return Row(previous.Status, previous.Win32, previous.Dll);

        if (!sameProfile)
            state.Failures = 0;
        var palette = mask == 0 ? DisabledPalette : Palette.Encode(mask, preset!.Preset.Colors);
        var result = await _apply(process.Identity, palette);
        state.AttemptedProfile = profile;
        state.Result = result;
        if (result.Status is "applied" or "disabled")
            state.OwnsActivePalette = mask != 0 && result.Status == "applied";
        if (result.Status == "not_ready")
        {
            state.Failures = Math.Min(state.Failures + 1, 5);
            state.RetryAt = _clock.GetUtcNow() + TimeSpan.FromSeconds(Math.Min(1 << state.Failures, 30));
        }
        return Row(result.Status, result.Win32, result.Dll);
    }

    private void Publish(MonitorSnapshot snapshot)
    {
        if (_lastSnapshot is { } previous && previous.ConfigurationError == snapshot.ConfigurationError &&
            previous.Enabled == snapshot.Enabled && previous.Processes.SequenceEqual(snapshot.Processes))
            return;
        _lastSnapshot = snapshot;
        SnapshotChanged?.Invoke(snapshot);
    }

    private async Task StopCoreAsync(bool restoreOwned)
    {
        _shutdown.Cancel();
        if (_polling is not null)
            await _polling.ConfigureAwait(false);
        _shutdown.Dispose();
        if (!restoreOwned)
            return;

        try
        {
            ForgetExitedTargets(_scan());
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException)
        {
            // Keep tracked lifetimes when the scanner cannot refresh them.
        }
        var failures = new List<string>();
        foreach (var (identity, state) in _targets)
        {
            var status = state.Result?.Status;
            if (status == "ambiguous" || (status == "conflict" && state.OwnsActivePalette))
            {
                failures.Add($"PID {identity.ProcessId} ({status})");
                continue;
            }
            if (!state.OwnsActivePalette || status == "stale")
                continue;
            var result = await _apply(identity, DisabledPalette).ConfigureAwait(false);
            if (result.Status is not ("disabled" or "stale"))
                failures.Add($"PID {identity.ProcessId} ({result.Status})");
        }
        if (failures.Count != 0)
            throw new InvalidOperationException("Не удалось вернуть штатные цвета: " + string.Join(", ", failures));
    }

    private void ForgetExitedTargets(IReadOnlyList<DetectedProcess> detected)
    {
        var known = detected.Select(process => process.Identity).ToHashSet();
        foreach (var identity in _targets.Keys.ToArray())
            if (!known.Contains(identity) && !known.Contains(new ProcessIdentity(identity.ProcessId, 0)))
                _targets.Remove(identity);
    }

    private static bool IsFileError(Exception error) => error is IOException or UnauthorizedAccessException or
        JsonException or InvalidDataException or ArgumentException or NotSupportedException;

    private static async Task<ApplyResult> RunHelperAsync(string helperPath, ProcessIdentity identity, string palette)
    {
        using var process = new Process();
        process.StartInfo = new ProcessStartInfo(helperPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true
        };
        process.StartInfo.ArgumentList.Add("--attach");
        process.StartInfo.ArgumentList.Add(identity.ProcessId.ToString(CultureInfo.InvariantCulture));
        process.StartInfo.ArgumentList.Add(identity.CreationFileTime.ToString(CultureInfo.InvariantCulture));
        process.StartInfo.ArgumentList.Add("--palette");
        process.StartInfo.ArgumentList.Add(palette);
        try
        {
            if (!process.Start())
                return new("error");
        }
        catch (Win32Exception error)
        {
            return new("error", unchecked((uint)error.NativeErrorCode));
        }

        using var timeout = new CancellationTokenSource(HelperTimeout);
        var buffer = new char[4096];
        try
        {
            var readOutput = process.StandardOutput.ReadBlockAsync(buffer.AsMemory(), timeout.Token).AsTask();
            await Task.WhenAll(readOutput, process.WaitForExitAsync(timeout.Token));
            var read = await readOutput;
            if (read == buffer.Length)
                return new("ambiguous");
            return ParseHelperResult(new string(buffer, 0, read), process.ExitCode);
        }
        catch (Exception error) when (error is OperationCanceledException or IOException or InvalidOperationException)
        {
            // The remote operation can still finish after a timeout. Retain this lifetime's terminal state.
            return new("ambiguous");
        }
    }

    internal static ApplyResult ParseHelperResult(string output, int exitCode)
    {
        try
        {
            using var json = JsonDocument.Parse(output);
            var root = json.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("status", out var status) || status.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty("win32", out var win32) || win32.ValueKind != JsonValueKind.Number ||
                !win32.TryGetUInt32(out var windowsCode) ||
                !root.TryGetProperty("dll", out var dll) || dll.ValueKind != JsonValueKind.Number ||
                !dll.TryGetUInt32(out var dllCode))
                return new("ambiguous");
            var value = status.GetString();
            var expectedCode = value switch
            {
                "applied" or "disabled" => 0,
                "error" => 1,
                "invalid_argument" => 2,
                "not_ready" => 3,
                "stale" => 4,
                "unsupported" => 5,
                "conflict" => 6,
                "ambiguous" => 7,
                _ => -1
            };
            return expectedCode >= 0 && exitCode == expectedCode
                ? new ApplyResult(value!, windowsCode, dllCode) : new("ambiguous");
        }
        catch (JsonException)
        {
            return new("ambiguous");
        }
    }
}
