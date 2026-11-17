using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json;
using ConfiguratorThemeSettings;

var checks = 0;
void Check(bool condition, string message)
{
    if (!condition)
        throw new Exception(message);
    checks++;
}
void Reject<T>(Action action, [CallerArgumentExpression(nameof(action))] string? expression = null)
    where T : Exception
{
    try
    {
        action();
    }
    catch (T)
    {
        checks++;
        return;
    }
    throw new InvalidOperationException($"Expected {typeof(T).Name} from {expression}");
}

var run = Path.Combine(AppContext.BaseDirectory, "fixtures", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(run);
Environment.SetEnvironmentVariable("ONEC_DARK_THEME_DATA", run);
var configPath = ThemeConfiguration.DefaultPath;
Check(configPath == Path.Combine(run, "config.json"), "environment data root");
var shippedNames = new[] { "dark", "graphite", "pink", "native" };
for (var i = 0; i < shippedNames.Length; i++)
{
    var file = PresetFile.Load(Path.Combine(AppContext.BaseDirectory, "presets", shippedNames[i] + ".json"));
    var builtin = Palette.BuiltIns[i];
    Check(file.Hash.Length == 64 && file.Preset.Name == builtin.Name && file.Preset.Mask == builtin.Mask
          && file.Preset.Colors.SequenceEqual(builtin.Colors), "packaged preset");
}

var dark = Palette.BuiltIns[0];
var headerZone = Palette.Zones.Single(zone => zone.Bit == 8192);
Check(headerZone.Label == "Рамки и заголовки панелей" && headerZone.Offset == 33 &&
      headerZone.Fields.SequenceEqual(["Фон рамки и заголовка", "Текст заголовка"]), "panel header editor fields");
var firstPath = Path.Combine(run, "presets", "first.json");
var secondPath = Path.Combine(run, "presets", "second.json");
var firstHash = PresetFile.Save(firstPath, dark, "");
var edited = dark with { Colors = [..dark.Colors] };
edited.Colors[33] = " 1122cc ";
edited.Colors[34] = "#99ff00";
var secondHash = PresetFile.Save(secondPath, edited, "");
var first = PresetFile.Load(firstPath);
var second = PresetFile.Load(secondPath);
Check(first.Hash == firstHash && second.Hash == secondHash && first.Hash != second.Hash, "round-trip hashes");
Check(first.Preset.Name == second.Preset.Name && first.Preset.Colors[33] != second.Preset.Colors[33], "same display name");
var cli = Palette.Encode(second.Preset.Mask, second.Preset.Colors);
Check(cli.StartsWith("16127:") && cli.Split(',').Length == 35 && cli.EndsWith(",1122CC,99FF00"),
      "normalized panel header colors and default mask are sent to CLI");
using (var saved = JsonDocument.Parse(File.ReadAllText(secondPath)))
    Check(saved.RootElement.GetProperty("paletteVersion").GetInt32() == 6, "saved v6");
Reject<IOException>(() => PresetFile.Save(firstPath, edited, secondHash));
Reject<IOException>(() => PresetFile.Save(firstPath, edited, ""));
Check(PresetFile.Load(firstPath).Hash == firstHash, "stale writes retained file");
var replaced = PresetFile.Save(firstPath, edited, firstHash);
Check(replaced == secondHash && PresetFile.Load(firstPath).Hash == replaced, "current hash replacement");
var sourceBytes = File.ReadAllBytes(firstPath);
Check(replaced == Convert.ToHexString(SHA256.HashData(sourceBytes)), "saved hash represents file bytes");
var largePresetPath = Path.Combine(run, "large-preset.json");
var boundedPreset = new byte[ThemeJson.MaxFileBytes];
boundedPreset.AsSpan().Fill((byte)' ');
"\uFEFF"u8.CopyTo(boundedPreset);
sourceBytes.CopyTo(boundedPreset, 3);
File.WriteAllBytes(largePresetPath, boundedPreset);
var boundedRead = PresetFile.Load(largePresetPath);
Check(boundedRead.Preset.Colors.SequenceEqual(second.Preset.Colors) &&
      boundedRead.Hash == Convert.ToHexString(SHA256.HashData(boundedPreset)), "bounded BOM read and exact snapshot hash");
using (var extra = new FileStream(largePresetPath, FileMode.Append))
    extra.WriteByte((byte)' ');
Reject<InvalidDataException>(() => PresetFile.Load(largePresetPath));
Reject<InvalidDataException>(() => PresetFile.Save(largePresetPath, dark, boundedRead.Hash));
Check(new FileInfo(largePresetPath).Length == ThemeJson.MaxFileBytes + 1, "oversized CAS retained file");
Reject<InvalidDataException>(() => ThemeJson.Write(firstPath, new string('x', ThemeJson.MaxFileBytes), replaced));
Check(PresetFile.Load(firstPath).Hash == replaced, "oversized output retained file");
Reject<JsonException>(() => PresetFile.Parse("{invalid"));
Reject<JsonException>(() => PresetFile.Parse("""{"schemaVersion":1,"schemaVersion":1}"""));
Reject<JsonException>(() => PresetFile.Parse("""{"schemaVersion":1,"name":"X","paletteVersion":6,"colors":[]}"""));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Mask = 16384 }));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Colors = dark.Colors[..^1] }));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Colors = [..dark.Colors[..^1], "zzzzzz"] }));

foreach (var (version, count, mask) in new[] { (1, 25, 511), (2, 29, 1023), (3, 30, 2047), (4, 31, 4095), (5, 33, 8191) })
{
    string Json(int selectedMask) => JsonSerializer.Serialize(new
    {
        schemaVersion = 1, paletteVersion = version, name = "legacy", mask = selectedMask,
        colors = Enumerable.Repeat("abcdef", count)
    });
    var migrated = PresetFile.Parse(Json(mask));
    Check(migrated.Mask == mask && migrated.Colors.Length == 35 &&
          migrated.Colors.Take(count).All(c => c == "#ABCDEF") &&
          migrated.Colors.Skip(count).SequenceEqual(dark.Colors.Skip(count)), "legacy normalization");
    Reject<InvalidDataException>(() => PresetFile.Parse(Json(mask + 1)));
}
var legacyStore = new PresetStore();
var legacyStoreColors = Enumerable.Repeat("#ABCDEF", 33).ToArray();
File.WriteAllText(legacyStore.Path, JsonSerializer.Serialize(new
{
    Version = 5, Presets = new[] { new Preset("Сохранённая", 7935, legacyStoreColors) }
}));
var stored = legacyStore.Load().Single();
Check(stored.Mask == 7935 && stored.Colors[..33].SequenceEqual(legacyStoreColors) &&
      stored.Colors[33..].SequenceEqual(["#1F2938", "#E5EBF5"]), "v5 collection keeps mask and appends header defaults");

var baseline = ThemeConfiguration.CreateDefault(configPath);
Check(baseline.Enabled && baseline.DefaultPresetFile == "presets/dark.json", "initial relative default");
var initialHash = ThemeConfiguration.Save(configPath, baseline, "");
var absolute = Path.Combine(run, "custom", "absolute.json");
var configured = baseline with
{
    DefaultPresetFile = "presets/first.json",
    Rules =
    [
        new(null, null, "Enterprise", "presets/mode.json"),
        new(null, "8.3.27.2325-x64", null, "presets/platform.json"),
        new(null, "8.3.27.2325-X64", "Enterprise", "presets/platform-mode.json"),
        new("server: ServerA \\ DB_A ", null, null, "presets/base.json"),
        new("Server:SERVERA\\DB_A", null, "Enterprise", "presets/base-mode.json"),
        new("server:servera\\db_a", "8.3.27.2325-x64", null, "presets/base-platform.json"),
        new("server:servera\\db_a", "8.3.27.2325-x64", "Enterprise", "presets/second.json"),
        new(@"file:C:\Test\Folder\..\", null, null, absolute)
    ]
};
var configuredHash = ThemeConfiguration.Save(configPath, configured, initialHash);
var loaded = ThemeConfiguration.Load(configPath);
Check(loaded.Hash == configuredHash && loaded.Hash != initialHash, "configuration CAS and roundtrip");
Check(loaded.Configuration.DefaultPresetFile == "presets/first.json" &&
      loaded.Configuration.Rules[3].BaseId == @"server:SERVERA\DB_A", "path provenance and normalized rule");
var baseId = @" server:serverA\db_a ";
var x64 = " 8.3.27.2325-X64 ";
var x86 = "8.3.27.2325-x86";
string Resolve(string? b, string? p, string? m) => loaded.Configuration.ResolvePresetFile(b, p, m);
void Expected(string? b, string? p, string? m, string filename) =>
    Check(Resolve(b, p, m) == Path.Combine(run, "presets", filename + ".json"), filename);
Expected(null, null, null, "first");
Expected(null, x64, "designer", "platform");
Expected(null, x86, "Enterprise", "mode");
Expected(null, x64, "Enterprise", "platform-mode");
Expected(baseId, x86, "Designer", "base");
Expected(baseId, x86, "Enterprise", "base-mode");
Expected(baseId, x64, "Designer", "base-platform");
Expected(baseId, x64, " enterprise ", "second");
Check(Resolve(@" FILE:c:\test\ ", x64, "Enterprise") == absolute, "absolute path and base priority");
Reject<FileNotFoundException>(() => PresetFile.Load(Resolve(null, x64, "Enterprise")));
Reject<IOException>(() => ThemeConfiguration.Save(configPath, baseline, initialHash));
Check(ThemeConfiguration.Load(configPath).Hash == configuredHash, "failed save kept config");
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, configured with
{
    Rules = [..configured.Rules, new(" server:servera\\DB_A", null, " enterprise ", "same-key.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with
{
    Rules = [new(@"file:C:\Test\", null, null, "one.json"), new(@"file:c:\test\.\", null, null, "two.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with
{
    Rules = [new(null, null, null, "catch-all.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with
{
    Rules = [new(null, "8.3.27-x64", null, "invalid-platform.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with
{
    Rules = [new(null, null, "ThinClient", "invalid-mode.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with
{
    Rules = [new("file:relative", null, null, "invalid-base.json")]
}));
Reject<InvalidDataException>(() => ThemeConfiguration.Save(configPath, baseline with { DefaultPresetFile = "C:partial.json" }));
Reject<JsonException>(() => ThemeConfiguration.Parse("""{"schemaVersion":1,"enabled":true,"defaultPresetFile":"a.json","rules":[],"unknown":0}""", configPath));
Reject<JsonException>(() => ThemeConfiguration.Parse("""{"schemaVersion":1,"enabled":true,"defaultPresetFile":"a.json","rules":[{"mode":"Designer"}]}""", configPath));
Check(!Directory.EnumerateFiles(run, "*.tmp", SearchOption.AllDirectories).Any(), "temporary files cleaned");
Console.WriteLine($"{checks} file and routing checks passed.");
