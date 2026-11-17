using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace ConfiguratorThemeSettings;

internal sealed record Zone(int Bit, string Label, int Offset, string[] Fields, string? Hint = null);
internal sealed record Preset(string Name, int Mask, string[] Colors);

internal static class Palette
{
    public const int Version = 7;
    public const int AllZones = 16383;
    public const int ColorCount = 36;
    public const int PanelHeaders = 8192;
    private const int DefaultZones = 7935 | PanelHeaders;
    public const int MaxPresetName = 60;
    public const string DefaultName = "Тёмная";
    public const string GraphiteName = "Графитовая";
    public const string PinkName = "Нежно-розовая";
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
        new(512, "Выпадающие меню", 25, ["Фон", "Полоса значков", "Текст", "Горячие клавиши"]),
        new(1024, "Свободная полоса за кнопками", 29, ["Фон"]),
        new(2048, "Рамки верхнего меню и групп кнопок", 30, ["Цвет рамок"]),
        new(4096, "Результаты поиска", 31, ["Фон", "Текст"]),
        new(PanelHeaders, "Рамки и заголовки панелей", 33,
            ["Фон рамки и заголовка", "Текст заголовка", "Наружная рамка панелей"],
            "Цвет контура дочерних окон и поддерживаемых рамок панелей.")
    ];

    private static readonly string[] Dark =
    [
        "#1F2938", "#80F2C2", "#1F2938", "#E5EBF5", "#1F2938", "#E5EBF5",
        "#1F2938", "#E5EBF5", "#A7B5C7", "#B9D4FF", "#305A8A", "#1F2938",
        "#1F2938", "#E5EBF5", "#A7B5C7", "#1F2938", "#E5EBF5", "#305A8A",
        "#2A374A", "#1F2938", "#E5EBF5", "#305A8A", "#2A374A", "#FFFFFF", "#000000",
        "#1F2938", "#2A374A", "#E5EBF5", "#878B8F", "#1F2938", "#3B4A60",
        "#1F2938", "#E5EBF5", "#1F2938", "#E5EBF5", "#3B4758"
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
            "#262629", "#3A373B", "#F3F0ED", "#8E8882", "#262629", "#464249",
            "#262629", "#F3F0ED", "#262629", "#F3F0ED", "#49454C"
        ]),
        new(PinkName, DefaultZones,
        [
            "#FFF4F7", "#523044", "#F8E1E9", "#3C2433", "#F9E7EE", "#3C2433",
            "#FFF4F7", "#3C2433", "#663B4F", "#70324F", "#E9AAC5", "#F7E9EF",
            "#F9E7EE", "#3C2433", "#663B4F", "#F9E7EE", "#3C2433", "#E9AAC5",
            "#F6DCE7", "#F9E7EE", "#3C2433", "#E9AAC5", "#F6DCE7", "#FFFFFF", "#000000",
            "#FFF4F7", "#F6DCE7", "#3C2433", "#663B4F", "#F9E7EE", "#B47C96",
            "#FFF4F7", "#3C2433", "#F8E1E9", "#3C2433", "#B87999"
        ]),
        new(NativeName, 0, Dark)
    ];

    public static (int MaxMask, int ColorCount) Shape(int version) => version switch
    {
        1 => (511, 25),
        2 => (1023, 29),
        3 => (2047, 30),
        4 => (4095, 31),
        5 => (8191, 33),
        6 => (16383, 35),
        Version => (AllZones, ColorCount),
        _ => throw new InvalidDataException("Неподдерживаемая версия палитры.")
    };

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
        if (imported is not { Version: >= 1 and <= Palette.Version, Presets: not null })
            throw new InvalidDataException("Неподдерживаемый формат файла пресетов.");
        var (maxMask, colorCount) = Palette.Shape(imported.Version);
        var presets = new List<Preset>(imported.Presets.Length);
        // Older files could already have a custom preset with the new built-in name.
        var names = new HashSet<string>(Palette.BuiltIns.Where(p => p.Name != Palette.PinkName).Select(p => p.Name),
                                        StringComparer.OrdinalIgnoreCase);
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
            presets.Add(colorCount < Palette.ColorCount
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
                JsonSerializer.Serialize(output, new Document(Palette.Version, [.. presets]), JsonOptions);
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
