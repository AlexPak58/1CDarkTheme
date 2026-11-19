using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ConfiguratorThemeSettings;

internal sealed record ThemeRule(string? BaseId, string? Platform, string? Mode,
                                 [property: JsonRequired] string PresetFile);

internal sealed record LoadedConfiguration(ThemeConfiguration Configuration, string Hash);

internal sealed record ThemeConfiguration(bool Enabled, string DefaultPresetFile,
                                          IReadOnlyList<ThemeRule> Rules, string ConfigurationPath)
{
    private const int SchemaVersion = 1;
    private const string DefaultPreset = "presets/dark.json";

    private sealed record Document(
        [property: JsonRequired] int SchemaVersion,
        [property: JsonRequired] bool Enabled,
        [property: JsonRequired] string DefaultPresetFile,
        [property: JsonRequired] ThemeRule[] Rules);

    public static string DefaultPath
    {
        get
        {
            var configured = Environment.GetEnvironmentVariable("ONEC_DARK_THEME_DATA");
            var directory = string.IsNullOrWhiteSpace(configured)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "1CDarkTheme")
                : Path.GetFullPath(configured);
            return Path.Combine(directory, "config.json");
        }
    }

    public static ThemeConfiguration CreateDefault(string path) =>
        new(true, DefaultPreset, [], Path.GetFullPath(path));

    public static LoadedConfiguration Load(string path)
    {
        var (document, hash) = ThemeJson.Read<Document>(path);
        return new LoadedConfiguration(Validate(document, path), hash);
    }

    public static ThemeConfiguration Parse(string json, string configurationPath) =>
        Validate(JsonSerializer.Deserialize<Document>(json, ThemeJson.Options), configurationPath);

    public static string Save(string path, ThemeConfiguration configuration, string? expectedHash = null)
    {
        var document = new Document(SchemaVersion, configuration.Enabled, configuration.DefaultPresetFile,
                                    [.. configuration.Rules]);
        var normalized = Validate(document, path);
        return ThemeJson.Write(path, document with { Rules = [.. normalized.Rules] }, expectedHash);
    }

    public string ResolvePresetFile(string? baseId, string? platform, string? mode)
    {
        baseId = NormalizeBaseId(baseId);
        platform = NormalizePlatform(platform);
        mode = NormalizeMode(mode);
        var selected = DefaultPresetFile;
        var selectedWeight = 0;
        foreach (var rule in Rules)
        {
            if (!Matches(rule.BaseId, baseId) || !Matches(rule.Platform, platform) || !Matches(rule.Mode, mode))
                continue;
            var weight = RuleWeight(rule);
            if (weight <= selectedWeight)
                continue;
            selected = rule.PresetFile;
            selectedWeight = weight;
        }
        return ResolvePath(selected, Path.GetDirectoryName(ConfigurationPath)!);
    }

    public static string? NormalizeBaseId(string? baseId)
    {
        if (baseId is null)
            return null;
        baseId = baseId.Trim();
        if (baseId.Any(char.IsControl))
            throw new InvalidDataException("Недопустимый идентификатор базы.");
        if (baseId.StartsWith("server:", StringComparison.OrdinalIgnoreCase))
        {
            var parts = baseId[7..].Split('\\', StringSplitOptions.TrimEntries);
            if (parts.Length == 2 && parts.All(part => part.Length > 0 && !part.Contains('/')))
                return "server:" + string.Join('\\', parts).ToUpperInvariant();
        }
        if (baseId.StartsWith("file:", StringComparison.OrdinalIgnoreCase))
        {
            var path = baseId[5..].Trim();
            if (Path.IsPathFullyQualified(path))
                return "file:" + Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)).ToUpperInvariant();
        }
        throw new InvalidDataException("Укажите базу в формате server:СЕРВЕР\\БАЗА или file:ПОЛНЫЙ_ПУТЬ.");
    }

    private static ThemeConfiguration Validate(Document? document, string path)
    {
        if (document is not { SchemaVersion: SchemaVersion, Rules: not null })
            throw new InvalidDataException("Неподдерживаемый формат файла настроек.");

        path = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(path)!;
        _ = ResolvePath(document.DefaultPresetFile, directory);
        var normalized = new List<ThemeRule>(document.Rules.Length);
        var keys = new HashSet<(string? Base, string? Platform, string? Mode)>();
        foreach (var rule in document.Rules)
        {
            if (rule is null)
                throw new InvalidDataException("Правило выбора пресета отсутствует.");
            var item = rule with
            {
                BaseId = NormalizeBaseId(rule.BaseId),
                Platform = NormalizePlatform(rule.Platform),
                Mode = NormalizeMode(rule.Mode)
            };
            if (RuleWeight(item) == 0)
                throw new InvalidDataException("Для правила укажите базу, платформу или режим.");
            if (!keys.Add((item.BaseId, item.Platform, item.Mode)))
                throw new InvalidDataException("Есть несколько правил для одной базы, платформы и режима.");
            _ = ResolvePath(item.PresetFile, directory);
            normalized.Add(item);
        }
        return new ThemeConfiguration(document.Enabled, document.DefaultPresetFile,
                                      normalized.AsReadOnly(), path);
    }

    private static int RuleWeight(ThemeRule rule) =>
        (rule.BaseId is null ? 0 : 4) + (rule.Platform is null ? 0 : 2) + (rule.Mode is null ? 0 : 1);

    private static bool Matches(string? condition, string? value) =>
        condition is null || string.Equals(condition, value, StringComparison.OrdinalIgnoreCase);

    private static string? NormalizePlatform(string? platform)
    {
        if (platform is null)
            return null;
        platform = platform.Trim().ToLowerInvariant();
        if (platform.EndsWith("-x64", StringComparison.Ordinal) ||
            platform.EndsWith("-x86", StringComparison.Ordinal))
        {
            var number = platform[..^4];
            if (Version.TryParse(number, out var version) && version.Revision >= 0 && version.ToString(4) == number)
                return platform;
        }
        throw new InvalidDataException("Укажите точную версию и разрядность платформы, например 8.3.27.2325-x64.");
    }

    private static string? NormalizeMode(string? mode)
    {
        if (mode is null)
            return null;
        return mode.Trim().ToUpperInvariant() switch
        {
            "DESIGNER" => "Designer",
            "ENTERPRISE" => "Enterprise",
            _ => throw new InvalidDataException("Режим должен быть Designer или Enterprise.")
        };
    }

    private static string ResolvePath(string path, string directory)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Any(char.IsControl) ||
            (Path.IsPathRooted(path) && !Path.IsPathFullyQualified(path)))
            throw new InvalidDataException("Укажите путь к файлу пресета.");
        return Path.GetFullPath(path, directory);
    }
}
