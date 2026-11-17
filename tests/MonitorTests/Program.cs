using ConfiguratorThemeSettings;

var checks = 0;
void Check(bool condition, string description)
{
    if (!condition)
        throw new InvalidOperationException(description);
    checks++;
}

var parserCases = new (string Args, bool Thin, string? Mode, string? Base)[]
{
    (@"""C:\Program Files\1cv8.exe"" dEsIgNeR /S""TestHost:1541\MainBase"" /SLev2", false,
        "Designer", "server:TESTHOST:1541\\MAINBASE"),
    (@"1cv8.exe ENTERPRISE /S ""TestHost\MainBase"" /N User /P ""Pa ss""", false,
        "Enterprise", "server:TESTHOST\\MAINBASE"),
    (@"1cv8.exe DESIGNER /N ""/SFake\Fake"" /P ""/FC:\Fake"" /F ""C:\Bases\Space Base""", false,
        "Designer", "file:C:\\BASES\\SPACE BASE"),
    ("""1cv8.exe DESIGNER /NUser /P"attached secret" /FC:\Bases\Test""", false,
        "Designer", "file:C:\\BASES\\TEST"),
    (@"1cv8.exe DESIGNER /F""\\Server\Share\Base""", false, "Designer", "file:\\\\SERVER\\SHARE\\BASE"),
    ("""1cv8.exe DESIGNER /SServer\Good /SOther\Other""", false, "Designer", null),
    ("""1cv8.exe DESIGNER /SServer\Good /FC:\Other""", false, "Designer", null),
    ("""1cv8.exe DESIGNER /F relative\test""", false, "Designer", null),
    ("""1cv8.exe DESIGNER /F C:relative""", false, "Designer", null),
    (@"1cv8.exe DESIGNER /SServer\Good /IBName ""Display Name""", false, "Designer", null),
    ("""1cv8.exe DESIGNER /SServer\Good /IBConnectionString Connection""", false, "Designer", null),
    ("""1cv8.exe DESIGNER /SServer\Good /WShttps://example.invalid/base""", false, "Designer", null),
    ("""1cv8.exe UNKNOWN /SServer\Test""", false, null, "server:SERVER\\TEST"),
    ("""1cv8c.exe /FC:\Bases\Client""", true, "Enterprise", "file:C:\\BASES\\CLIENT"),
    ("""1cv8c.exe DESIGNER /FC:\Bases\Client""", true, "Enterprise", "file:C:\\BASES\\CLIENT")
};
for (var i = 0; i < parserCases.Length; i++)
{
    var item = parserCases[i];
    Check(OneCArguments.Parse(item.Args, item.Thin) == (item.Mode, item.Base), $"Argument case {i + 1}");
}

Check(ProcessMonitor.ParseHelperResult("""{"status":"applied","win32":0,"dll":0}""", 0).Status == "applied",
    "Helper accepts agreed status and exit code");
Check(ProcessMonitor.ParseHelperResult("""{"status":"applied","win32":0,"dll":0}""", 1).Status == "ambiguous",
    "Helper mismatched exit code fails closed");
Check(ProcessMonitor.ParseHelperResult("""{"status":"applied","win32":0,"dll":0}{}""", 0).Status == "ambiguous",
    "Helper accepts a single result");
Check(ProcessMonitor.ParseHelperResult("""{"status":"not_ready","win32":5,"dll":312}""", 3) == new ApplyResult("not_ready", 5, 312),
    "Helper preserves numeric results");

var directory = Path.Combine(AppContext.BaseDirectory, "fixtures", Guid.NewGuid().ToString("N"));
var configPath = Path.Combine(directory, "config.json");
var defaultPath = Path.Combine(directory, "default.json");
var overridePath = Path.Combine(directory, "override.json");
var dark = Palette.BuiltIns[0];
var light = Palette.BuiltIns[2];
PresetFile.Save(defaultPath, dark);
PresetFile.Save(overridePath, light);
var config = ThemeConfiguration.CreateDefault(configPath) with { DefaultPresetFile = "default.json" };
ThemeConfiguration.Save(configPath, config);

const string platform = "8.3.27.2325-x64";
var designer = new DetectedProcess(new ProcessIdentity(120, 4), "Designer", platform, null);
var enterprise = new DetectedProcess(new ProcessIdentity(121, 5), "Enterprise", platform, null);
var otherBuild = new DetectedProcess(new ProcessIdentity(122, 6), "Designer", "8.3.27.2325-x86", null);
var clock = new TestClock();
IReadOnlyList<DetectedProcess> candidates = [designer, enterprise, otherBuild];
var requests = new List<(ProcessIdentity, string)>();
var reply = new ApplyResult("applied");
var monitor = new ProcessMonitor(configPath, false, () => candidates, (identity, palette) =>
{
    requests.Add((identity, palette));
    return Task.FromResult(palette.StartsWith("0:", StringComparison.Ordinal) ? new ApplyResult("disabled") : reply);
}, clock);
MonitorSnapshot? snapshot = null;
var changes = 0;
monitor.SnapshotChanged += current => { snapshot = current; changes++; };

await monitor.PollOnceAsync();
Check(requests.Count == 1 && requests[0] == (designer.Identity, Palette.Encode(dark.Mask, dark.Colors)),
    "Only supported Designer gets the default file profile");
Check(snapshot!.Processes.Select(item => item.Status).SequenceEqual(["applied", "unsupported", "unsupported"]),
    "Unsupported mode and platform are visible");
Check(snapshot.Processes[0].PresetPath == defaultPath && snapshot.Processes[0].PresetName == dark.Name,
    "Status includes resolved preset file and name");
await monitor.PollOnceAsync();
Check(requests.Count == 1 && changes == 1, "Unchanged successful profile is neither reapplied nor republished");

var updatedDark = dark with { Colors = [..dark.Colors] };
updatedDark.Colors[35] = "#124578";
PresetFile.Save(defaultPath, updatedDark);
await monitor.PollOnceAsync();
Check(requests.Count == 2 && requests[^1].Item2 == Palette.Encode(updatedDark.Mask, updatedDark.Colors),
    "Changed panel border color at the same path reapplies");

config = config with { Rules = [new(null, platform, "Designer", "override.json")] };
ThemeConfiguration.Save(configPath, config);
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.Processes[0].PresetPath == overridePath,
    "Editing the effective rule selects and applies another file");
File.WriteAllText(overridePath, "{");
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.Processes[0].Status == "preset_error" && snapshot.Processes[0].PresetPath == overridePath,
    "Invalid selected file retains the previous palette and surfaces the error");
File.WriteAllText(overridePath, """{"schemaVersion":0,"name":"Invalid","paletteVersion":6,"mask":0,"colors":[]}""");
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.Processes[0].Status == "preset_error", "Invalid preset schema suspends writes");
File.Delete(overridePath);
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.Processes[0].Status == "preset_error", "Missing selected file does not fall back");
File.WriteAllText(configPath, "{");
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.ConfigurationError is not null && snapshot.Processes[0].Status == "config_error",
    "Invalid config suspends writes");
File.WriteAllText(configPath, """{"schemaVersion":0,"enabled":true,"defaultPresetFile":"default.json","rules":[]}""");
await monitor.PollOnceAsync();
Check(requests.Count == 3 && snapshot.Processes[0].Status == "config_error", "Invalid config schema suspends writes");

ThemeConfiguration.Save(configPath, config with { Enabled = false });
await monitor.PollOnceAsync();
await monitor.PollOnceAsync();
Check(requests.Count == 4 && requests[^1].Item2.StartsWith("0:") && snapshot.Processes[0].Status == "disabled",
    "Disabled config restores native colors even with a missing selected file");
var newDesigner = designer with { Identity = new ProcessIdentity(123, 7) };
candidates = [designer, newDesigner];
await monitor.PollOnceAsync();
Check(requests.Count == 5 && requests[^1].Item1 == newDesigner.Identity && requests[^1].Item2.StartsWith("0:"),
    "Disabled startup can detach a palette left by an earlier monitor");

PresetFile.Save(overridePath, light);
ThemeConfiguration.Save(configPath, config);
candidates = [designer];
reply = new("not_ready");
await monitor.PollOnceAsync();
await monitor.PollOnceAsync();
Check(requests.Count == 6 && snapshot.Processes[0].Status == "not_ready", "Not ready honors backoff");
clock.Advance(TimeSpan.FromSeconds(2));
reply = new("ambiguous");
await monitor.PollOnceAsync();
Check(requests.Count == 7 && snapshot.Processes[0].Status == "ambiguous", "Not ready retried once when backoff elapsed");
clock.Advance(TimeSpan.FromMinutes(1));
ThemeConfiguration.Save(configPath, config with { Rules = [] });
await monitor.PollOnceAsync();
Check(requests.Count == 7 && snapshot.Processes[0].Status == "ambiguous", "Ambiguous lifetime blocks retry after config edit");
candidates = [designer with { Identity = new ProcessIdentity(120, 0), DiscoveryStatus = "access_denied" }];
await monitor.PollOnceAsync();
candidates = [designer];
await monitor.PollOnceAsync();
Check(requests.Count == 7, "Temporary failure to open a target preserves its ambiguous lifetime");
reply = new("applied");
var reusedPid = designer with { Identity = new ProcessIdentity(designer.Identity.ProcessId, 22) };
candidates = [reusedPid];
await monitor.PollOnceAsync();
Check(requests.Count == 8 && requests[^1].Item1 == reusedPid.Identity, "PID reuse is a new lifetime");
await monitor.StopAsync(restoreOwned: true);
Check(requests.Count == 9 && requests[^1].Item1 == reusedPid.Identity && requests[^1].Item2.StartsWith("0:"),
    "Exit restores only the owned active lifetime");

var release = new TaskCompletionSource<ApplyResult>(TaskCreationOptions.RunContinuationsAsynchronously);
var serialCalls = 0;
var serial = new ProcessMonitor(configPath, false, () => [designer, newDesigner], (_, _) =>
    ++serialCalls == 1 ? release.Task : Task.FromResult(new ApplyResult("applied")), clock);
var running = serial.PollOnceAsync();
Check(serialCalls == 1 && !running.IsCompleted, "Second helper waits until the first completes");
release.SetResult(new ApplyResult("applied"));
await running;
Check(serialCalls == 2, "Second target is applied after release");
await serial.StopAsync();

var observer = new ProcessMonitor(configPath, true, () => [designer], (_, _) =>
    throw new InvalidOperationException("Observation attempted to apply a palette"), clock);
observer.SnapshotChanged += current => snapshot = current;
await observer.PollOnceAsync();
Check(snapshot.Processes[0].Status == "observed", "Observation mode lists an eligible process without attaching");
await observer.StopAsync(restoreOwned: true);

var terminalCalls = 0;
var terminalMonitor = new ProcessMonitor(configPath, false, () => [designer], (_, _) =>
    Task.FromResult(++terminalCalls == 1 ? new ApplyResult("applied") : new ApplyResult("ambiguous")), clock);
await terminalMonitor.PollOnceAsync();
PresetFile.Save(defaultPath, dark);
await terminalMonitor.PollOnceAsync();
var restoreReported = false;
try
{
    await terminalMonitor.StopAsync(restoreOwned: true);
}
catch (InvalidOperationException error)
{
    restoreReported = error.Message.Contains("PID 120 (ambiguous)", StringComparison.Ordinal);
}
Check(restoreReported && terminalCalls == 2, "Exit reports an unrestored ambiguous lifetime without another attach");

var firstAttemptCalls = new List<(ProcessIdentity, string)>();
var firstAmbiguous = new ProcessMonitor(configPath, false, () => [designer, newDesigner], (identity, palette) =>
{
    firstAttemptCalls.Add((identity, palette));
    return Task.FromResult(identity == designer.Identity ? new ApplyResult("ambiguous") :
        new ApplyResult(palette.StartsWith("0:", StringComparison.Ordinal) ? "disabled" : "applied"));
}, clock);
await firstAmbiguous.PollOnceAsync();
await firstAmbiguous.PollOnceAsync();
restoreReported = false;
try
{
    await firstAmbiguous.StopAsync(restoreOwned: true);
}
catch (InvalidOperationException error)
{
    restoreReported = error.Message.Contains("PID 120 (ambiguous)", StringComparison.Ordinal);
}
Check(restoreReported && firstAttemptCalls.Count == 3 && firstAttemptCalls[^1].Item1 == newDesigner.Identity &&
    firstAttemptCalls[^1].Item2.StartsWith("0:", StringComparison.Ordinal),
    "Exit reports first-attempt ambiguity and restores the other active target");

IReadOnlyList<DetectedProcess> exiting = [designer];
var exitedCalls = 0;
var exitedAmbiguous = new ProcessMonitor(configPath, false, () => exiting, (_, _) =>
{
    exitedCalls++;
    return Task.FromResult(new ApplyResult("ambiguous"));
}, clock);
await exitedAmbiguous.PollOnceAsync();
exiting = [];
await exitedAmbiguous.StopAsync(restoreOwned: true);
Check(exitedCalls == 1, "Exit clears an ambiguous target that already terminated");

var conflictCalls = 0;
var unownedConflict = new ProcessMonitor(configPath, false, () => [designer], (_, _) =>
{
    conflictCalls++;
    return Task.FromResult(new ApplyResult("conflict"));
}, clock);
await unownedConflict.PollOnceAsync();
await unownedConflict.StopAsync(restoreOwned: true);
Check(conflictCalls == 1, "Exit leaves an unowned conflict alone");

Console.WriteLine($"{checks} monitor checks passed.");

internal sealed class TestClock : TimeProvider
{
    private DateTimeOffset _utc = DateTimeOffset.UnixEpoch;
    public override DateTimeOffset GetUtcNow() => _utc;
    public void Advance(TimeSpan delta) => _utc += delta;
}
