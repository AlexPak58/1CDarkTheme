using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ConfiguratorThemeSettings;

internal sealed record LoadedPreset(Preset Preset, string Hash);

internal static class PresetFile
{
    private const int SchemaVersion = 1;
    private const int PaletteVersion = 5;

    private sealed record Document(
        [property: JsonRequired] int SchemaVersion,
        [property: JsonRequired] string Name,
        [property: JsonRequired] int PaletteVersion,
        [property: JsonRequired] int Mask,
        [property: JsonRequired] string[] Colors);

    public static LoadedPreset Load(string path)
    {
        var (document, hash) = ThemeJson.Read<Document>(path);
        return new LoadedPreset(Validate(document), hash);
    }

    public static Preset Parse(string json) =>
        Validate(JsonSerializer.Deserialize<Document>(json, ThemeJson.Options));

    public static string Save(string path, Preset preset, string? expectedHash = null)
    {
        var normalized = Normalize(preset.Name, preset.Mask, preset.Colors, PaletteVersion);
        return ThemeJson.Write(path, new Document(SchemaVersion, normalized.Name, PaletteVersion,
                                                  normalized.Mask, normalized.Colors), expectedHash);
    }

    private static Preset Validate(Document? document)
    {
        if (document is not { SchemaVersion: SchemaVersion })
            throw new InvalidDataException("Неподдерживаемый формат файла пресета.");
        return Normalize(document.Name, document.Mask, document.Colors, document.PaletteVersion);
    }

    private static Preset Normalize(string name, int mask, string[] colors, int version)
    {
        var (maxMask, colorCount) = version switch
        {
            1 => (511, 25),
            2 => (1023, 29),
            3 => (2047, 30),
            4 => (4095, 31),
            PaletteVersion => (Palette.AllZones, Palette.ColorCount),
            _ => throw new InvalidDataException("Неподдерживаемая версия палитры.")
        };
        if (!Palette.IsValidName(name) || mask < 0 || mask > maxMask || colors?.Length != colorCount)
            throw new InvalidDataException("Недопустимое имя, набор зон или число цветов в пресете.");

        var normalized = new string[Palette.ColorCount];
        for (var i = 0; i < colorCount; i++)
            if (!Palette.TryNormalizeColor(colors[i], out normalized[i]))
                throw new InvalidDataException("В пресете записан недопустимый цвет.");
        Palette.BuiltIns[0].Colors.AsSpan(colorCount).CopyTo(normalized.AsSpan(colorCount));
        return new Preset(name, mask, normalized);
    }
}

internal static class ThemeJson
{
    internal const int MaxFileBytes = 1024 * 1024;

    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        AllowDuplicateProperties = false,
        WriteIndented = true
    };

    public static (T? Document, string Hash) Read<T>(string path)
    {
        var snapshot = ReadSnapshot(path);
        ReadOnlySpan<byte> json = snapshot;
        if (json.StartsWith("\uFEFF"u8))
            json = json[3..];
        return (JsonSerializer.Deserialize<T>(json, Options), Hash(snapshot));
    }

    // null permits an unconditional write; an empty hash expects a new file.
    public static string Write<T>(string path, T document, string? expectedHash)
    {
        var json = JsonSerializer.SerializeToUtf8Bytes(document, Options);
        CheckSize(json.Length);
        path = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var gate = new FileStream(path + ".lock", FileMode.OpenOrCreate, FileAccess.Write, FileShare.None);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                output.Write(json);
                output.Flush(flushToDisk: true);
            }
            if (expectedHash is not null && expectedHash != GetCurrentHash(path))
                throw new IOException("Файл изменился после открытия. Загрузите его заново.");
            File.Move(temporary, path, overwrite: true);
            return Hash(json);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private static byte[] ReadSnapshot(string path)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        var length = input.Length;
        CheckSize(length);
        using var snapshot = new MemoryStream((int)length);
        Span<byte> buffer = stackalloc byte[8192];
        while (true)
        {
            // Read one extra byte to detect growth past the limit.
            var count = input.Read(buffer[..Math.Min(buffer.Length, MaxFileBytes - (int)snapshot.Length + 1)]);
            if (count == 0)
                return snapshot.ToArray();
            CheckSize(snapshot.Length + count);
            snapshot.Write(buffer[..count]);
        }
    }

    private static void CheckSize(long length)
    {
        if (length > MaxFileBytes)
            throw new InvalidDataException("Размер файла настроек или пресета превышает 1 МиБ.");
    }

    private static string Hash(ReadOnlySpan<byte> input) => Convert.ToHexString(SHA256.HashData(input));

    private static string GetCurrentHash(string path)
    {
        try
        {
            return Hash(ReadSnapshot(path));
        }
        catch (FileNotFoundException)
        {
            return "";
        }
    }
}
