using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security;
using System.Text.Json;

namespace ConfiguratorThemeSettings;

internal static class LegacyPresetImport
{
    private const string SourceWarning = "Не удалось прочитать старые пресеты. Новые настройки доступны; исходный файл не изменён.";

    // Optional compatibility import: a bad old file must not prevent the new agent from starting.
    public static string? Import(string sourcePath, string destinationDirectory)
    {
        try
        {
            using var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            if (source.Length > ThemeJson.MaxFileBytes)
                throw new InvalidDataException("Размер старого файла превышает допустимый предел.");
            using var document = JsonDocument.Parse(source);
            var version = document.RootElement.GetProperty("Version").GetInt32();
            _ = Palette.Shape(version);
            var items = document.RootElement.GetProperty("Presets");
            if (items.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("Ожидался массив старых пресетов.");
            var index = 0;
            var failed = 0;
            foreach (var item in items.EnumerateArray())
            {
                var destination = Path.Combine(destinationDirectory, $"legacy-{++index:D3}.json");
                try
                {
                    if (File.Exists(destination)) continue;
                    var colors = item.GetProperty("Colors").EnumerateArray().Select(value => value.GetString()).ToArray();
                    var json = JsonSerializer.Serialize(new
                    {
                        schemaVersion = 1,
                        name = item.GetProperty("Name").GetString(),
                        paletteVersion = version,
                        mask = item.GetProperty("Mask").GetInt32(),
                        colors
                    });
                    var preset = PresetFile.Parse(json);
                    // Empty expected hash requires a new file; a concurrent writer wins unchanged.
                    PresetFile.Save(destination, preset, "");
                }
                catch (Exception error) when (IsImportError(error)) { failed++; }
            }
            return failed == 0 ? null : $"Не удалось импортировать старые пресеты: {failed}. Остальные импортированы; исходный файл не изменён.";
        }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException) { return null; }
        catch (Exception error) when (IsImportError(error)) { return SourceWarning; }
    }

    private static bool IsImportError(Exception error) => error is IOException or InvalidDataException or UnauthorizedAccessException
        or SecurityException or JsonException or InvalidOperationException or KeyNotFoundException
        or FormatException or OverflowException or ArgumentException or NotSupportedException;
}

