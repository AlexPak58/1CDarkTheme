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
    var packagePath = Path.Combine(AppContext.BaseDirectory, "presets", shippedNames[i] + ".json");
    var file = PresetFile.Load(packagePath);
    var builtin = Palette.BuiltIns[i];
    Check(file.Hash.Length == 64 && file.Preset.Name == builtin.Name && file.Preset.Mask == builtin.Mask
          && file.Preset.Colors.SequenceEqual(builtin.Colors), "packaged preset");
    Check(file.Preset.Mask == (i == 3 ? 0 : 32511), "packaged splitter zone default");
    using var packaged = JsonDocument.Parse(File.ReadAllText(packagePath));
    Check(packaged.RootElement.GetProperty("schemaVersion").GetInt32() == 1 &&
          packaged.RootElement.GetProperty("paletteVersion").GetInt32() == 8, "packaged v8 profile");
}

var dark = Palette.BuiltIns[0];
Check(dark.Colors[36] == "#3B4758", "dark splitter color");
var headerZone = Palette.Zones.Single(zone => zone.Bit == 8192);
Check(headerZone.Label == "Рамки и заголовки панелей" && headerZone.Offset == 33 &&
      headerZone.Fields.SequenceEqual(["Фон рамки и заголовка", "Текст заголовка", "Наружная рамка панелей"]),
      "panel header and border editor fields");
var splitterZone = Palette.Zones.Single(zone => zone.Bit == 16384);
Check(splitterZone.Label == "Разделители панелей" && splitterZone.Offset == 36 &&
      splitterZone.Fields.SequenceEqual(["Фон разделителей панелей"]), "splitter editor field");
var firstPath = Path.Combine(run, "presets", "first.json");
var secondPath = Path.Combine(run, "presets", "second.json");
var firstHash = PresetFile.Save(firstPath, dark, "");
var edited = dark with { Colors = [..dark.Colors] };
edited.Colors[33] = " 1122cc ";
edited.Colors[34] = "#99ff00";
edited.Colors[35] = " 3a4b5c ";
edited.Colors[36] = " 558ac3 ";
var secondHash = PresetFile.Save(secondPath, edited, "");
var first = PresetFile.Load(firstPath);
var second = PresetFile.Load(secondPath);
Check(first.Hash == firstHash && second.Hash == secondHash && first.Hash != second.Hash, "round-trip hashes");
Check(first.Preset.Name == second.Preset.Name && first.Preset.Colors[33] != second.Preset.Colors[33], "same display name");
var cli = Palette.Encode(second.Preset.Mask, second.Preset.Colors);
Check(cli.StartsWith("32511:") && cli.Split(',').Length == 37 && cli.EndsWith(",1122CC,99FF00,3A4B5C,558AC3"),
      "normalized panel and splitter colors and default mask are sent to CLI");
using (var saved = JsonDocument.Parse(File.ReadAllText(secondPath)))
    Check(saved.RootElement.GetProperty("schemaVersion").GetInt32() == 1 &&
          saved.RootElement.GetProperty("paletteVersion").GetInt32() == 8, "saved v8 profile");
foreach (var mask in new[] { 0, 16384, 32767 })
{
    var splitterPath = Path.Combine(run, $"splitter-{mask}.json");
    PresetFile.Save(splitterPath, edited with { Mask = mask });
    var splitter = PresetFile.Load(splitterPath).Preset;
    Check(splitter.Mask == mask && splitter.Colors[36] == "#558AC3" &&
          splitter.Colors.Take(36).SequenceEqual(second.Preset.Colors.Take(36)),
          $"splitter color persists with mask {mask}");
}
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
Reject<JsonException>(() => PresetFile.Parse("""{"schemaVersion":1,"name":"X","paletteVersion":7,"colors":[]}"""));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Mask = 32768 }));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Colors = dark.Colors[..^1] }));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Colors = [..dark.Colors, "#AABBCC"] }));
Reject<InvalidDataException>(() => PresetFile.Save(firstPath, dark with { Colors = [..dark.Colors[..^1], "zzzzzz"] }));

string[] LegacyColors(int count) => Enumerable.Range(0, count).Select(i => $"#{0xA10000 + i:X6}").ToArray();
foreach (var (version, count, mask) in new[]
    { (1, 25, 511), (2, 29, 1023), (3, 30, 2047), (4, 31, 4095), (5, 33, 8191), (6, 35, 16383), (7, 36, 16383) })
{
    var colors = LegacyColors(count);
    string Json(int selectedMask, int colorCount) => JsonSerializer.Serialize(new
    {
        schemaVersion = 1, paletteVersion = version, name = "legacy", mask = selectedMask,
        colors = LegacyColors(colorCount).Select(c => c[1..].ToLowerInvariant())
    });
    var migrated = PresetFile.Parse(Json(mask, count));
    Check(migrated.Mask == mask && migrated.Colors.Length == 37 &&
          migrated.Colors.Take(count).SequenceEqual(colors) && migrated.Colors[36] == "#3B4758" &&
          migrated.Colors.Skip(count).SequenceEqual(dark.Colors.Skip(count)), $"v{version} legacy normalization");
    Check(PresetFile.Parse(Json(0, count)).Mask == 0, "legacy native colors do not enable zones");
    Reject<InvalidDataException>(() => PresetFile.Parse(Json(mask + 1, count)));
    Reject<InvalidDataException>(() => PresetFile.Parse(Json(mask, count + 1)));
}
var legacyStore = new PresetStore();
foreach (var (version, mask, colors, appended) in new[]
{
    (5, 7935, LegacyColors(33), new[] { "#1F2938", "#E5EBF5", "#3B4758", "#3B4758" }),
    (6, 8192, LegacyColors(35), new[] { "#3B4758", "#3B4758" }),
    (7, 0x2241, LegacyColors(36), new[] { "#3B4758" })
})
{
    File.WriteAllText(legacyStore.Path, JsonSerializer.Serialize(new
    {
        Version = version, Presets = new[] { new Preset("Сохранённая", mask, colors) }
    }));
    var stored = legacyStore.Load().Single();
    Check(stored.Mask == mask && stored.Colors.SequenceEqual(colors.Concat(appended)),
          $"v{version} collection keeps mask and appends defaults");
}

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
