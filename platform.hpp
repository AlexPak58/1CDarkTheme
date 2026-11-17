#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <string>
#include <string_view>

inline constexpr ULONG_PTR enableTreeOnly = 1;
inline constexpr ULONG_PTR enableTreeAndCaption = 2;
inline constexpr ULONG_PTR enableTreeAndBars = 4;
inline constexpr ULONG_PTR enableTreeAndProperties = 8;
inline constexpr ULONG_PTR enableWorkspace = 16;
inline constexpr ULONG_PTR enableStatusBar = 32;
inline constexpr ULONG_PTR enableWindowTabs = 64;
inline constexpr ULONG_PTR enableTreeTabs = 128;
inline constexpr ULONG_PTR enableDropdown = 256;
inline constexpr ULONG_PTR enablePopupMenu = 512;
inline constexpr ULONG_PTR enableToolbarBand = 1024;
inline constexpr ULONG_PTR enableToolbarBorder = 2048;
inline constexpr ULONG_PTR enableSearchResults = 4096;
inline constexpr ULONG_PTR enablePanelHeaders = 8192;
inline constexpr ULONG_PTR enableDockSplitter = 16384;
inline constexpr DWORD themeZoneMask = 32767;
inline constexpr DWORD themeProfileVersion = 8;

enum PaletteColor : size_t {
    treeBackground, treeText, captionBackground, captionText,
    barsBackground, barsText,
    propertyBackground, propertyText, propertyMutedText, propertyGroupText, propertySelection,
    workspaceBackground, statusBackground, statusText, statusMutedText,
    windowTabsBackground, windowTabsText, windowTabsActive, windowTabsInactive,
    treeTabsBackground, treeTabsText, treeTabsActive, treeTabsInactive,
    dropdownBackground, dropdownText,
    popupMenuBackground, popupMenuGutter, popupMenuText, popupMenuShortcut,
    toolbarBandBackground, toolbarBorder,
    searchResultsBackground, searchResultsText,
    panelHeaderBackground, panelHeaderText, panelBorder, dockSplitterFill,
    paletteColorCount
};

// Versioned payload copied across the launcher/Configurator process boundary.
struct ThemeProfile {
    DWORD size{sizeof(ThemeProfile)};
    DWORD version{themeProfileVersion};
    DWORD zones{255 | enablePopupMenu | enableToolbarBand | enableToolbarBorder | enableSearchResults |
                enablePanelHeaders | enableDockSplitter};
    std::array<DWORD, paletteColorCount> colors{
        0x1F2938, 0x80F2C2, 0x1F2938, 0xE5EBF5, 0x1F2938, 0xE5EBF5,
        0x1F2938, 0xE5EBF5, 0xA7B5C7, 0xB9D4FF, 0x305A8A, 0x1F2938,
        0x1F2938, 0xE5EBF5, 0xA7B5C7, 0x1F2938, 0xE5EBF5, 0x305A8A, 0x2A374A,
        0x1F2938, 0xE5EBF5, 0x305A8A, 0x2A374A, 0xFFFFFF, 0x000000,
        0x1F2938, 0x2A374A, 0xE5EBF5, 0x878B8F, 0x1F2938, 0x3B4A60,
        0x1F2938, 0xE5EBF5, 0x1F2938, 0xE5EBF5, 0x3B4758, 0x3B4758
    };
};

static_assert(sizeof(ThemeProfile) == (paletteColorCount + 3) * sizeof(DWORD));

constexpr DWORD toBgr(DWORD rgb)
{
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

inline constexpr auto exeHash =
    L"4C8E2F497CCC03747F60B3C6CC82350030C84484F6ED8AA9E40671FEACAF9BC6";
inline constexpr auto frameHash =
    L"D2F4367C1F4DE24747440A4CF963961585BDF0D854A3C324741F912DB123F2D7";
inline constexpr auto graphicsHash =
    L"125C00C01E7780704AD3E2159170DD7E1A05EF8CF9B2ED32B0B30CB535C4A563";

inline std::wstring modulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(module, path.data(),
                                          static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    path.resize(length);
    return path;
}

using Sha256Hex = std::array<wchar_t, 65>;

inline bool readSha256(const std::wstring& path, Sha256Hex& hash)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    const bool validLength = GetFileSizeEx(file, &length) &&
                             length.QuadPart > 0 &&
                             length.QuadPart <= std::numeric_limits<ULONG>::max();
    const HANDLE mapping = validLength ?
        CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    const auto* bytes = mapping ?
        static_cast<const UCHAR*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    std::array<UCHAR, 32> digest{};
    const bool hashed = bytes &&
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<UCHAR*>(bytes),
                   static_cast<ULONG>(length.QuadPart), digest.data(),
                   static_cast<ULONG>(digest.size())) == 0;
    if (bytes) UnmapViewOfFile(bytes);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    if (!hashed) return false;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (size_t i = 0; i < digest.size(); ++i) {
        hash[2 * i] = hex[digest[i] >> 4];
        hash[2 * i + 1] = hex[digest[i] & 15];
    }
    hash.back() = L'\0';
    return true;
}

inline bool matchesSha256(const std::wstring& path, const wchar_t* expected)
{
    Sha256Hex hash{};
    return readSha256(path, hash) && std::wstring_view(hash.data()) == expected;
}
