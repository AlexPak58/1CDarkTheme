#include "enterprise_navigation.hpp"
#include <cstdio>
#include <vector>

namespace {
int failed{}, assertions{}, calls{};
void* chosen{};
enterprise_navigation::PatternCreate createPattern{};
void check(bool value, const char* message) {
    ++assertions;
    if (!value) { ++failed; std::fprintf(stderr, "FAIL %s\n", message); }
}
void* pattern(void* surface) {
    ++calls; chosen = surface;
    check(GetLastError() == 1234, "incoming LastError preserved");
    auto* result = createPattern(surface);
    SetLastError(4321);
    return result;
}
template<class T> T symbol(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::fputs("Usage: enterprise-navigation.exe <pinned cairo.dll> <exported PNG folder>\n", stderr); return 2; }
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module || !enterprise_navigation::initialize(module)) return 2;
    auto fromPng = symbol<void*(*)(const char*)>(module, "cairo_image_surface_create_from_png");
    auto destroyPattern = symbol<void(*)(void*)>(module, "cairo_pattern_destroy");
    auto getSurface = symbol<int(*)(void*, void**)>(module, "cairo_pattern_get_surface");
    createPattern = symbol<enterprise_navigation::PatternCreate>(module, "cairo_pattern_create_for_surface");
    if (!fromPng || !destroyPattern || !getSurface || !createPattern) return 2;
    auto& api = enterprise_navigation::api;
    ThemeProfile profile{};
    profile.colors[barsBackground] = 0x202428; profile.colors[barsText] = 0xDFE5EB;
    WIN32_FIND_DATAW file{};
    const std::wstring directory = argv[2];
    const auto find = FindFirstFileW((directory + L"\\*.png").c_str(), &file);
    if (find == INVALID_HANDLE_VALUE) return 2;
    int count{};
    do {
        const std::wstring path = directory + L"\\" + file.cFileName;
        const auto length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::vector<char> utf8(length);
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), length, nullptr, nullptr);
        for (int variant = 0; variant < 2; ++variant) {
        auto* source = fromPng(utf8.data());
        check(source && !api.surface.status(source), "real PNG decoded");
        if (!source || api.surface.status(source)) { if (source) api.surface.destroy(source); continue; }
        ++count;
        api.setScale(source, 1.25, 1.5); api.setOffset(source, -2.5, 3.75);
        thumb_png::detail::Image image{};
        check(thumb_png::detail::describe(api.surface, source, image), "surface metadata valid");
        auto* bytes = api.surface.data(source);
        if (variant == 1) {
            // Independent resource decoder exported the exact 1C floor variant.
            // Copy it into a genuine Cairo image, preserving its stride.
            const HANDLE fixture = CreateFileW((path + L".floorbgra").c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            std::vector<unsigned char> floor(static_cast<size_t>(image.width) * image.height * 4);
            DWORD read{}; LARGE_INTEGER size{};
            const bool loaded = fixture != INVALID_HANDLE_VALUE && GetFileSizeEx(fixture, &size) &&
                size.QuadPart == static_cast<LONGLONG>(floor.size()) &&
                ReadFile(fixture, floor.data(), static_cast<DWORD>(floor.size()), &read, nullptr) && read == floor.size();
            if (fixture != INVALID_HANDLE_VALUE) CloseHandle(fixture);
            check(loaded && image.format == thumb_png::detail::argb32, "floor premultiplication fixture loaded");
            if (!loaded) { api.surface.destroy(source); continue; }
            for (int y = 0; y < image.height; ++y)
                std::memcpy(bytes + static_cast<size_t>(y) * image.stride,
                            floor.data() + static_cast<size_t>(y) * image.width * 4, image.width * 4);
            api.surface.markDirty(source);
        }
        const auto* asset=enterprise_navigation::findAsset(source);
        const bool expectedSubsystem=std::wstring_view(file.cFileName).starts_with(L"defTopSubsystem");
        check(asset && (asset->skin==enterprise_navigation::detail::Skin::subsystem)==expectedSubsystem,
              "exact rounded/floor platform pixels admitted with the correct role");
        const std::vector<unsigned char> before(bytes, bytes + image.bytes);
        SetLastError(1234); calls = 0;
        auto* result = enterprise_navigation::patternForSurface(pattern, source, profile);
        check(GetLastError() == 4321 && calls == 1, "outgoing error and single original invocation");
        check(chosen != source, "known asset cloned");
        void* held{};
        check(result && !getSurface(result, &held) && held == chosen && !api.surface.status(held), "pattern owns surviving clone");
        thumb_png::detail::Image cloned{};
        check(thumb_png::detail::describe(api.surface, held, cloned) && cloned.scaleX == 1.25 &&
              cloned.scaleY == 1.5 && cloned.offsetX == -2.5 && cloned.offsetY == 3.75, "scale and offset retained");
        check(!std::memcmp(bytes, before.data(), image.bytes), "borrowed platform cache unchanged");
        bool recolored{};
        const auto* themed = api.surface.data(held);
        for (int row=0;row<image.height;++row) {
            if (std::memcmp(themed + static_cast<size_t>(row)*cloned.stride,
                            before.data() + static_cast<size_t>(row)*image.stride,image.width*4)) {
                recolored=true;
                break;
            }
        }
        check(recolored,"skin recolored");
        destroyPattern(result);
        // RGB24 upper byte is undefined and must not affect identity.
        if (image.format == thumb_png::detail::rgb24) {
            for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x)
                bytes[y * image.stride + x * 4 + 3] ^= 0xA5;
            api.surface.markDirty(source);
            check(enterprise_navigation::matches(source), "RGB24 high byte ignored");
        }
        bytes[0] ^= 1; api.surface.markDirty(source);
        check(!enterprise_navigation::matches(source), "one-bit mutation rejected");
        calls = 0; SetLastError(1234);
        result = enterprise_navigation::patternForSurface(pattern, source, profile);
        check(calls == 1 && chosen == source && GetLastError() == 4321, "unknown image passes through");
        destroyPattern(result); api.surface.destroy(source);
        }
    } while (FindNextFileW(find, &file));
    FindClose(find);
    check(count == 96, "all 48 navigation and subsystem assets tested in both premultiplication variants");
    check(!enterprise_navigation::matches(nullptr) && !enterprise_navigation::matches(reinterpret_cast<void*>(1)), "invalid surfaces rejected safely");
    std::printf("%s: %d assets, %d assertions, %d failures\n", failed ? "FAIL" : "PASS", count, assertions, failed);
    return failed ? 1 : 0;
}
