using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace ConfiguratorThemeSettings;

internal sealed record Zone(int Bit, string Label, int Offset, string[] Fields);
internal sealed record Preset(string Name, int Mask, string[] Colors);

internal static class Palette
{
    public const int LegacyZones = 511;
    public const int LegacyColorCount = 25;
    public const int AllZones = 1023;
    public const int ColorCount = 29;
    private const int DefaultZones = 767;
    public const int MaxPresetName = 60;
    public const string DefaultName = "Тёмная";
    public const string GraphiteName = "Графитовая";
    public const string NativeName = "Штатные цвета";

    // Bits and color offsets match ThemeProfile and PaletteColor in platform.hpp.
    public static readonly Zone[] Zones =
    [
        new(1, "Деревья конфигурации и расширений", 0, ["Фон", "Текст"]),
        new(2, "Заголовок окна", 2, ["Фон", "Текст"]),
        new(4, "Верхнее меню и панели", 4, ["Фон", "Текст"]),
        new(8, "Палитра свойств", 6, ["Фон", "Текст", "Приглушённый текст", "Заголовки групп", "Фон выделенной строки"]),
        new(16, "Рабочая область", 11, ["Фон"]),
        new(32, "Строка состояния", 12, ["Фон", "Текст", "Приглушённый текст"]),
        new(64, "Вкладки открытых окон", 15, ["Фон панели", "Текст", "Активная вкладка", "Неактивная вкладка"]),
        new(128, "Вкладки под деревом", 19, ["Фон панели", "Текст", "Активная вкладка", "Неактивная вкладка"]),
        new(256, "Выпадающие списки (эксперимент)", 23, ["Фон", "Текст"]),
        new(512, "Выпадающие меню", 25, ["Фон", "Полоса значков", "Текст", "Горячие клавиши"])
    ];

    private static readonly string[] Dark =
    [
        "#1F2938", "#80F2C2", "#1F2938", "#E5EBF5", "#1F2938", "#E5EBF5",
        "#1F2938", "#E5EBF5", "#A7B5C7", "#B9D4FF", "#305A8A", "#1F2938",
        "#1F2938", "#E5EBF5", "#A7B5C7", "#1F2938", "#E5EBF5", "#305A8A",
        "#2A374A", "#1F2938", "#E5EBF5", "#305A8A", "#2A374A", "#FFFFFF", "#000000",
        "#1F2938", "#2A374A", "#E5EBF5", "#878B8F"
    ];

    public static readonly Preset[] BuiltIns =
    [
        new(DefaultName, DefaultZones, Dark),
        new(GraphiteName, DefaultZones,
        [
            "#262629", "#F3CF93", "#262629", "#F3F0ED", "#262629", "#F3F0ED",
            "#262629", "#F3F0ED", "#BDB8B2", "#F3CF93", "#614742", "#262629",
            "#262629", "#F3F0ED", "#BDB8B2", "#262629", "#F3F0ED", "#614742",
            "#3A373B", "#262629", "#F3F0ED", "#614742", "#3A373B", "#FFFFFF", "#000000",
            "#262629", "#3A373B", "#F3F0ED", "#8E8882"
        ]),
        new(NativeName, 0, Dark)
    ];

    public static bool TryNormalizeColor(string? value, out string normalized)
    {
        normalized = "";
        var input = value?.Trim();
        if (string.IsNullOrEmpty(input))
            return false;
        if (input.StartsWith('#'))
            input = input[1..];
        if (input.Length != 6 || !input.All(Uri.IsHexDigit))
            return false;
        normalized = "#" + input.ToUpperInvariant();
        return true;
    }

    public static bool IsValidName(string? name) => name is { Length: > 0 and <= MaxPresetName }
        && name == name.Trim() && !name.Any(char.IsControl);

    public static string Encode(int mask, IEnumerable<string> colors) =>
        mask.ToString(CultureInfo.InvariantCulture) + ":" + string.Join(',', colors.Select(c => c[1..]));
}

internal sealed class PresetStore
{
    private const int CurrentVersion = 2;
    private sealed record Document(int Version, Preset[] Presets);
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private string? _loadedHash;

    public string Path { get; } = GetPath();

    public List<Preset> Load()
    {
        using var input = OpenRead();
        if (input is null)
        {
            _loadedHash = null;
            return [];
        }
        var loadedHash = Hash(input);
        input.Position = 0;
        var imported = JsonSerializer.Deserialize<Document>(input, JsonOptions);
        if (imported is not { Version: 1 or CurrentVersion, Presets: not null })
            throw new InvalidDataException("Неподдерживаемый формат файла пресетов.");
        var legacy = imported.Version == 1;
        var maxMask = legacy ? Palette.LegacyZones : Palette.AllZones;
        var colorCount = legacy ? Palette.LegacyColorCount : Palette.ColorCount;
        var presets = new List<Preset>(imported.Presets.Length);
        var names = new HashSet<string>(Palette.BuiltIns.Select(p => p.Name), StringComparer.OrdinalIgnoreCase);
        foreach (var preset in imported.Presets)
        {
            if (preset is null || !Palette.IsValidName(preset.Name) || !names.Add(preset.Name)
                || preset.Mask < 0 || preset.Mask > maxMask || preset.Colors?.Length != colorCount)
                throw new InvalidDataException("Недопустимое имя, набор зон или число цветов.");
            for (var i = 0; i < colorCount; i++)
            {
                if (!Palette.TryNormalizeColor(preset.Colors[i], out var normalized))
                    throw new InvalidDataException("В пресете записан недопустимый цвет.");
                preset.Colors[i] = normalized;
            }
            presets.Add(legacy
                ? preset with { Colors = [.. preset.Colors, .. Palette.BuiltIns[0].Colors.AsSpan(colorCount)] }
                : preset);
        }
        _loadedHash = loadedHash;
        return presets;
    }

    public void Save(IEnumerable<Preset> presets)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        using var gate = new FileStream(Path + ".lock", FileMode.OpenOrCreate, FileAccess.Write, FileShare.None);
        using (var current = OpenRead())
        {
            var currentHash = current is null ? null : Hash(current);
            if (currentHash != _loadedHash)
                throw new IOException("Пресеты изменились в другом окне. Откройте настройки заново.");
        }
        var temporary = Path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            string savedHash;
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                JsonSerializer.Serialize(output, new Document(CurrentVersion, [.. presets]), JsonOptions);
                output.Flush(flushToDisk: true);
            }
            using (var saved = File.OpenRead(temporary))
                savedHash = Hash(saved);
            File.Move(temporary, Path, overwrite: true);
            _loadedHash = savedHash;
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private FileStream? OpenRead()
    {
        try
        {
            return new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException)
        {
            return null;
        }
    }

    private static string Hash(Stream stream) => Convert.ToHexString(SHA256.HashData(stream));

    private static string GetPath()
    {
        var configured = Environment.GetEnvironmentVariable("ONEC_DARK_THEME_DATA");
        var directory = string.IsNullOrWhiteSpace(configured)
            ? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                    "ConfiguratorThemeSettings")
            : System.IO.Path.GetFullPath(configured);
        return System.IO.Path.Combine(directory, "presets.json");
    }
}
