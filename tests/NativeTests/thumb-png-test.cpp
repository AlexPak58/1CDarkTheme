#include "thumb_png.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template<class T> T symbol(HMODULE module, const char* name)
{
    auto result = GetProcAddress(module, name);
    check(result != nullptr, name);
    return reinterpret_cast<T>(result);
}
thumb_png::CairoApi api;
scroller_png::ImageCreate realCreate;
scroller_png::SurfaceAction realDestroy, realDirty;
scroller_png::GetInt realStatus;
using CreateData = void* (*)(BYTE*, int, int, int, int);
CreateData createData;
void* sourceSurface{};
void* destinationSurface{};
int destroys{}, allocations{};
enum class Failure { None, Null, Source, Pixels, Dirty, Error, Padding, Transform };
Failure failure{};
std::array<DWORD, 256 * 260> destinationBytes{};
void* create(int format, int width, int height)
{
    ++allocations;
    if (failure == Failure::Null) return nullptr;
    if (failure == Failure::Source) return sourceSurface;
    if (failure == Failure::Pixels)
        destinationSurface = createData(api.surface.data(sourceSurface), format, width, height, api.surface.stride(sourceSurface));
    else if (failure == Failure::Padding) {
        destinationBytes.fill(0xA5A5A5A5);
        destinationSurface = createData(reinterpret_cast<BYTE*>(destinationBytes.data()), format, width, height, width * 4 + 16);
    } else destinationSurface = realCreate(failure == Failure::Error ? -1 : format, width, height);
    return destinationSurface;
}
void destroy(void* surface)
{
    check(surface != sourceSurface, "never release borrowed source");
    ++destroys; realDestroy(surface);
}
bool marked{};
void dirty(void* surface) { marked = true; realDirty(surface); }
int status(const void* surface)
{
    return failure == Failure::Dirty && marked && surface == destinationSurface ? 1 : realStatus(surface);
}
thumb_png::SetPair realSetScale;
void setScale(void* surface, double x, double y)
{
    if (failure != Failure::Transform) realSetScale(surface, x, y);
}
constexpr thumb_png::Palette palette{0x102030, 0xE0D0C0};
DWORD expected(DWORD input, bool argb)
{
    const DWORD a = argb ? input >> 24 : 255;
    DWORD straight{};
    for (unsigned shift = 0; shift <= 16; shift += 8)
        straight |= (a ? (((input >> shift) & 255) * 255 + a / 2) / a : 0) << shift;
    const DWORD luma = (77 * ((straight >> 16) & 255) + 150 * ((straight >> 8) & 255) + 29 * (straight & 255) + 128) / 256;
    const DWORD weight = luma >= 240 ? 0 : luma <= 120 ? 120 : 240 - luma;
    DWORD result = argb ? a << 24 : 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        const auto channel = ((((palette.background >> shift) & 255) * (120 - weight) +
                              ((palette.detail >> shift) & 255) * weight + 60) / 120);
        result |= ((channel * a + 127) / 255) << shift;
    }
    return result;
}
void fixtures()
{
    using PatternCreate = void* (*)(void*);
    using PatternGet = int (*)(void*, void**);
    const auto module = GetModuleHandleW(L"cairo.dll");
    const auto patternCreate = symbol<PatternCreate>(module, "cairo_pattern_create_for_surface");
    const auto patternGet = symbol<PatternGet>(module, "cairo_pattern_get_surface");
    const auto patternDestroy = symbol<scroller_png::SurfaceAction>(module, "cairo_pattern_destroy");
    int cases{};
    for (const int format : {0, 1}) for (const double scale : {1.0, 1.25, 2.0})
    for (const int major : {16, 20}) for (const int minor : {1, 5, 6, 7}) {
        if ((major == 16 && minor == 7) || (major == 20 && minor == 5)) continue;
        for (const bool horizontal : {false, true}) {
            const int width = horizontal ? minor : major, height = horizontal ? major : minor;
            const int stride = width * 4 + 16;
            std::vector<DWORD> pixels(static_cast<size_t>(stride / 4 * height), 0xFEEDBEEF);
            const DWORD samplesArgb[]{0, 0x401E1E1E, 0x803C3C3C, 0xFF787878, 0xFFB4B4B4, 0xFFF0F0F0};
            const DWORD samplesRgb[]{0x00F0F0F0, 0xFFF0F0F0, 0x80787878, 0x40787878, 0xAAB4B4B4, 0x01D2D2D2};
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                pixels[static_cast<size_t>(y * stride / 4 + x)] = (format == 0 ? samplesArgb : samplesRgb)[(x + y) % 6];
            const auto before = pixels;
            sourceSurface = createData(reinterpret_cast<BYTE*>(pixels.data()), format, width, height, stride);
            check(sourceSurface && !realStatus(sourceSurface), "create real padded source");
            realSetScale(sourceSurface, scale, scale);
            api.setOffset(sourceSurface, -2.5, 3.75);
            marked = false;
            auto* clone = thumb_png::clone(api, sourceSurface, palette);
            check(clone && clone != sourceSurface && !realStatus(clone), "real Cairo clone");
            check(pixels == before, "all source pixels and row padding immutable");
            thumb_png::detail::Image info{};
            check(thumb_png::detail::describe(api.surface, clone, info) && info.format == format &&
                info.width == width && info.height == height && info.scaleX == scale && info.scaleY == scale &&
                info.offsetX == -2.5 && info.offsetY == 3.75, "format geometry and transforms preserved");
            api.surface.flush(clone);
            auto* output = api.surface.data(clone);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                DWORD actual{};
                std::memcpy(&actual, output + static_cast<size_t>(y * info.stride + x * 4), 4);
                check(actual == expected(pixels[static_cast<size_t>(y * stride / 4 + x)], format == 0), "premultiplied channels and exact alpha");
            }
            auto* pattern = patternCreate(clone);
            check(pattern != nullptr, "create pattern from owned clone");
            realDestroy(clone);
            realDestroy(sourceSurface); sourceSurface = nullptr;
            void* retained{};
            check(!patternGet(pattern, &retained) && retained == clone && !realStatus(retained), "pattern retains clone after owner release");
            api.surface.flush(retained);
            DWORD retainedPixel{};
            std::memcpy(&retainedPixel, api.surface.data(retained), 4);
            check(retainedPixel == expected(pixels.front(), format == 0), "pattern clone survives source and owner release");
            patternDestroy(pattern);
            ++cases;
        }
    }
    std::printf("PASS: %d real Cairo padded RGB24/ARGB32 shape/scale/offset and pattern lifetime fixtures\n", cases);
}
int invalidInteger(const void*) { return -1; }
int oversizedInteger(const void*) { return 257; }
int oddStride(const void*) { return 13; }
BYTE* inaccessible{};
BYTE* badSourceData(void* surface)
{
    return surface == sourceSurface ? inaccessible : api.surface.data(surface);
}
void failures()
{
    std::array<DWORD, 12> pixels{};
    pixels.fill(0x803C3C3C);
    const auto before = pixels;
    sourceSurface = createData(reinterpret_cast<BYTE*>(pixels.data()), 0, 2, 3, 16);
    realSetScale(sourceSurface, 1.25, 2);
    for (const auto mode : {Failure::Null, Failure::Source, Failure::Pixels, Failure::Dirty, Failure::Error, Failure::Transform}) {
        failure = mode; destroys = allocations = 0; marked = false;
        check(!thumb_png::clone(api, sourceSurface, palette), "failed clone falls back");
        check(allocations == 1 && destroys == ((mode == Failure::Null || mode == Failure::Source) ? 0 : 1), "release only distinct owned failure surface");
        check(pixels == before, "failure cannot mutate borrowed source");
    }
    failure = Failure::Padding; marked = false;
    auto* padded = thumb_png::clone(api, sourceSurface, palette);
    check(padded != nullptr, "padded destination accepted");
    for (size_t row = 0; row < 3; ++row) for (size_t x = 2; x < 6; ++x)
        check(destinationBytes[row * 6 + x] == 0xA5A5A5A5, "destination row padding untouched");
    realDestroy(padded);
    failure = Failure::None;
    const auto normal = api;
    for (const auto getter : {&scroller_png::CairoApi::type, &scroller_png::CairoApi::format,
                              &scroller_png::CairoApi::width, &scroller_png::CairoApi::height,
                              &scroller_png::CairoApi::stride}) {
        auto invalid = normal;
        invalid.surface.*getter = invalidInteger;
        allocations = 0;
        check(!thumb_png::clone(invalid, sourceSurface, palette) && allocations == 0, "unknown or negative image metadata rejected before allocation");
    }
    auto invalid = normal;
    invalid.surface.width = oversizedInteger;
    check(!thumb_png::clone(invalid, sourceSurface, palette), "bounded image dimensions");
    invalid = normal; invalid.surface.stride = oddStride;
    check(!thumb_png::clone(invalid, sourceSurface, palette), "unaligned row stride rejected");
    invalid = normal; invalid.surface.data = badSourceData;
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto* storage = static_cast<BYTE*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(storage != nullptr, "allocate bounded data fixture");
    DWORD prior{};
    check(VirtualProtect(storage + page, page, PAGE_READWRITE | PAGE_GUARD, &prior) != FALSE, "protect data boundary");
    for (const auto pointer : {storage + page, storage + page - 4}) {
        inaccessible = pointer; allocations = 0;
        check(!thumb_png::clone(invalid, sourceSurface, palette) && allocations == 0, "guarded and out-of-range source data rejected before allocation");
    }
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(storage + page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "guard flag never consumed");
    check(VirtualFree(storage, 0, MEM_RELEASE) != FALSE, "release boundary fixture");
    pixels[0] = 0x40000041; marked = false;
    check(!thumb_png::clone(api, sourceSurface, palette) && !marked, "invalid premultiplication rejected");
    pixels[0] = 1;
    check(!thumb_png::clone(api, sourceSurface, palette), "alpha zero nonzero RGB rejected");
    pixels[0] = before[0];
    for (const auto bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        realSetScale(sourceSurface, bad, 1);
        check(!thumb_png::clone(api, sourceSurface, palette), "invalid scale rejected");
    }
    realSetScale(sourceSurface, 1, 1);
    for (const auto bad : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        api.setOffset(sourceSurface, bad, 0);
        check(!thumb_png::clone(api, sourceSurface, palette), "nonfinite offset rejected");
    }
    realDestroy(sourceSurface); sourceSurface = nullptr;
    auto missing = api; missing.setOffset = nullptr;
    check(!thumb_png::clone(missing, reinterpret_cast<void*>(1), palette) &&
          !thumb_png::clone(api, reinterpret_cast<void*>(1), palette), "missing API and inaccessible surface fail closed");
    std::puts("PASS: failure ownership, overlapping pixels, dirty/error/transform failures, destination padding, invalid alpha/scale/API");
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fputs("Usage: thumb-png-test.exe <explicit pinned cairo.dll path>\n", stderr);
        return 2;
    }
    check(matchesSha256(argv[1], cairoHash), "explicit pinned Cairo DLL hash");
    auto module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    check(module != nullptr, "load Cairo into isolated test process");
    auto& a = api.surface;
    a.status = symbol<scroller_png::GetInt>(module, "cairo_surface_status"); realStatus = a.status; a.status = status;
    a.type = symbol<scroller_png::GetInt>(module, "cairo_surface_get_type");
    a.format = symbol<scroller_png::GetInt>(module, "cairo_image_surface_get_format");
    a.width = symbol<scroller_png::GetInt>(module, "cairo_image_surface_get_width");
    a.height = symbol<scroller_png::GetInt>(module, "cairo_image_surface_get_height");
    a.stride = symbol<scroller_png::GetInt>(module, "cairo_image_surface_get_stride");
    a.scale = symbol<scroller_png::GetPair>(module, "cairo_surface_get_device_scale");
    a.offset = symbol<scroller_png::GetPair>(module, "cairo_surface_get_device_offset");
    a.flush = symbol<scroller_png::SurfaceAction>(module, "cairo_surface_flush");
    realDestroy = symbol<scroller_png::SurfaceAction>(module, "cairo_surface_destroy"); a.destroy = destroy;
    realDirty = symbol<scroller_png::SurfaceAction>(module, "cairo_surface_mark_dirty"); a.markDirty = dirty;
    a.data = symbol<scroller_png::GetData>(module, "cairo_image_surface_get_data");
    realCreate = symbol<scroller_png::ImageCreate>(module, "cairo_image_surface_create"); a.imageCreate = create;
    realSetScale = symbol<thumb_png::SetPair>(module, "cairo_surface_set_device_scale"); api.setScale = setScale;
    api.setOffset = symbol<thumb_png::SetPair>(module, "cairo_surface_set_device_offset");
    createData = symbol<CreateData>(module, "cairo_image_surface_create_for_data");
    check(expected(0x00000000, true) == 0 && expected(0x401E1E1E, true) == 0x40383430 &&
          expected(0x803C3C3C, true) == 0x80706860 && expected(0xFF787878, true) == 0xFFE0D0C0,
          "independently fixed alpha 0/64/128/255 palette endpoints");
    fixtures(); failures();
    check(FreeLibrary(module) != FALSE, "release test DLL");
}
