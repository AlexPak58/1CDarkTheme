#include "scroller_png.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
using namespace scroller_png;

void check(bool condition, const char* reason)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}

template<class T, size_t N>
void put(std::array<ULONG_PTR, N>& storage, size_t offset, T value)
{
    check(offset <= sizeof(storage) && sizeof(value) <= sizeof(storage) - offset, "fixture bounds");
    std::memcpy(reinterpret_cast<BYTE*>(storage.data()) + offset, &value, sizeof(value));
}

template<size_t N>
ULONG_PTR address(const std::array<ULONG_PTR, N>& storage)
{
    return reinterpret_cast<ULONG_PTR>(storage.data());
}

struct PaintFixture {
    static constexpr ULONG_PTR graphics = 0x20000000, frame = 0x10000000;
    static constexpr size_t c = 0x4C0, e = 0x550, trackS = 0x610, arrowS = 0x6A0;
    std::array<ULONG_PTR, 0x800 / 8> stack{};
    std::array<ULONG_PTR, 0x110 / 8> scroller{};
    std::array<ULONG_PTR, 0xC0 / 8> window{};
    std::array<ULONG_PTR, 0x18 / 8> component{};
    std::unique_ptr<std::array<ULONG_PTR, 0x40000 / 8>> tables =
        std::make_unique<std::array<ULONG_PTR, 0x40000 / 8>>();
    const ULONG_PTR wbase{address(*tables)};

    void route(DWORD part, bool tree, DWORD offset)
    {
        stack.fill(0);
        const auto layoutOffset = 0xA8 + offset;
        const auto slotOffset = 0xA8 + 0x20 + (offset - 0x40) / 2;
        const auto s = part == 3 ? trackS : arrowS;
        put(stack, 0, graphics + 0x13482);
        put(stack, 0x260, graphics + 0x4911);
        put(stack, 0x430, graphics + 0x91BA);
        put(stack, c, wbase + (part == 3 ? 0x19410 : 0x1963F));
        if (part != 3) {
            put(stack, e, wbase + (part == 0 ? 0x19452 : 0x1948B));
            put(stack, e - 8, address(scroller) + 0x28);
            put(stack, e + 0x30, DWORD{0xFF000000});
            put(stack, e + 0x38, BYTE{1});
        }
        put(stack, part == 3 ? c - 8 : e - 0x10, address(component));
        put(stack, s, wbase + 0x1E9E0);
        put(stack, s + 0x90, wbase + 0x1D5D9);
        put(stack, s + 0x28, BYTE{1});
        put(stack, s - 0x38, address(scroller));
        put(stack, s - 0x40, address(scroller) + 0xA8);
        put(stack, s - 0x20, static_cast<ULONG_PTR>(offset));
        put(stack, s - 0x10, address(scroller) + slotOffset);
        put(stack, s - 0x18, address(scroller) + layoutOffset);
        put(scroller, 0, wbase + (tree ? 0x3CD00 : 0x3CA78));
        put(scroller, 0x20, address(window));
        put(scroller, 0x98, BYTE{1});
        put(scroller, slotOffset, address(component));
        put(scroller, layoutOffset, RECT{-50, 3, -22, 47});
        put(window, 0, frame + (tree ? 0xA885D0 : 0xABEDB0));
        put(window, 0xB8, address(scroller));
        put(*tables, 0x3CA78 + 0x58, wbase + 0x1D600);
        put(*tables, 0x3CA78 + 0xC0, wbase + 0x1E860);
        put(*tables, 0x3CD00 + 0x58, wbase + 0x1D600);
        put(*tables, 0x3CD00 + 0xC0, wbase + 0x1EFE0);
        put(component, 0, wbase + 0x3D348);
        put(component, 8, (offset - 0x40) / 0x10);
        put(component, 0xC, DWORD{1});
        put(component, 0x10, DWORD{2});
        put(*tables, 0x3D348 + 0x10, wbase + 0x19290);
        put(*tables, 0x3D348 + 0x30, wbase + 0x1B930);
    }

    bool matches(Identity& id) const { return identify(address(stack), frame, graphics, wbase, id); }
};

void testMatcher()
{
    PaintFixture f;
    Identity id;
    for (const DWORD part : {0UL, 1UL, 3UL}) {
        for (DWORD i = 0; i < 3; ++i) {
            const DWORD axis = i == 2 ? 1 : 0;
            f.route(part, i == 0, 0x40 + axis * 0x10);
            check(f.matches(id) && id.part == part && id.orientation == axis &&
                  id.hotId == 1 && id.pressedId == 2 && id.enabled && id.interactive,
                  "all nine accepted routes");
        }
    }
    f.route(3, true, 0x50);
    check(!f.matches(id), "tree horizontal is outside the accepted routes");
    for (size_t offset : {0U, 0x260U, 0x430U}) {
        f.route(3, false, 0x40);
        ++f.stack[offset / 8];
        check(!f.matches(id), "graphics return witnesses required");
    }
    f.route(3, false, 0x40);
    put(f.stack, PaintFixture::c, f.wbase + 0x19859);
    check(!f.matches(id), "thumb staging excluded");
    f.route(1, false, 0x40);
    ++f.stack[PaintFixture::e / 8];
    check(!f.matches(id), "unknown arrow return rejected");
    for (const size_t offset : {0x00U, 0x90U, 0x28U}) {
        f.route(3, false, 0x40);
        ++f.stack[(PaintFixture::trackS + offset) / 8];
        check(!f.matches(id), "outer return and draw mode witnesses required");
    }
    for (const size_t offset : {0x38U, 0x40U, 0x20U, 0x18U, 0x10U}) {
        f.route(3, false, 0x40);
        ++f.stack[(PaintFixture::trackS - offset) / 8];
        check(!f.matches(id), "scroller, layout, offset, rect and slot witnesses required");
    }
    for (const size_t offset : {0U, 0xB8U}) {
        f.route(3, false, 0x40);
        ++f.window[offset / 8];
        check(!f.matches(id), "Window table and backlink required");
    }
    for (const size_t offset : {0x58U, 0xC0U}) {
        f.route(3, true, 0x40);
        ++(*f.tables)[(0x3CD00 + offset) / 8];
        check(!f.matches(id), "scroller entry and dispatch witnesses required");
    }
    for (const size_t offset : {PaintFixture::c - 8, PaintFixture::e - 0x10}) {
        f.route(offset == PaintFixture::c - 8 ? 3 : 0, false, 0x40);
        ++f.stack[offset / 8];
        check(!f.matches(id), "component must match its slot");
    }
    for (const size_t offset : {0U, 8U, 0xCU, 0x10U}) {
        f.route(3, false, 0x40);
        put(f.component, offset, DWORD{MAXDWORD});
        check(!f.matches(id), "component type and state IDs checked");
    }
    for (const size_t offset : {0x10U, 0x30U}) {
        f.route(3, false, 0x40);
        ++(*f.tables)[(0x3D348 + offset) / 8];
        check(!f.matches(id), "component paint and getter witnesses required");
    }
    f.route(3, false, 0x40);
    put(f.scroller, 0xE8, RECT{1, 1, 1, 1});
    check(!f.matches(id) && !identify(panel_detail::limit - 7, f.frame, f.graphics, f.wbase, id),
          "empty rectangle and unsafe stack rejected");
}

void testArrowFlags()
{
    PaintFixture f;
    Identity id;
    constexpr auto e = PaintFixture::e;
    for (const DWORD part : {0UL, 1UL}) {
        f.route(part, false, 0x40);
        put(f.stack, e + 0xE8 + (1 - part), BYTE{1});
        check(f.matches(id) && id.enabled && id.interactive, "other arrow's disabled flag is ignored");
        put(f.stack, e + 0xE8 + part, BYTE{1});
        check(f.matches(id) && !id.enabled && id.interactive, "own arrow's disabled flag wins");
    }
    f.route(0, false, 0x40);
    put(f.scroller, 0x98, BYTE{0});
    check(f.matches(id) && !id.enabled && id.interactive, "scroller settings can disable both arrows");
    f.route(1, false, 0x40);
    put(f.stack, e + 0x38, BYTE{0});
    check(f.matches(id) && id.enabled && !id.interactive, "interaction is independent of the overwritten enabled argument");
    put(f.stack, e - 8, ULONG_PTR{1});
    check(!f.matches(id), "saved configuration must belong to the matched scroller");

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto* pages = static_cast<BYTE*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "allocate guarded flag fixture");
    f.route(0, false, 0x40);
    auto* object = pages + page - 0x98;
    std::memcpy(object, f.scroller.data(), sizeof(f.scroller));
    const auto scroller = reinterpret_cast<ULONG_PTR>(object);
    constexpr auto s = PaintFixture::arrowS;
    put(f.stack, s - 0x38, scroller);
    put(f.stack, s - 0x40, scroller + 0xA8);
    put(f.stack, s - 0x10, scroller + 0xC8);
    put(f.stack, s - 0x18, scroller + 0xE8);
    put(f.stack, e - 8, scroller + 0x28);
    put(f.window, 0xB8, scroller);
    check(f.matches(id) && id.enabled && id.interactive, "relocated flag fixture matches before protection");
    DWORD prior{};
    check(VirtualProtect(pages + page, page, PAGE_READWRITE | PAGE_GUARD, &prior) != FALSE,
          "guard page containing the configuration flag");
    check(!f.matches(id), "unreadable configuration flag declines the route");
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(pages + page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
          "guard remains set after flag lookup");
    check(VirtualFree(pages, 0, MEM_RELEASE) != FALSE, "release guarded flag fixture");
}

struct Surface {
    int status{}, type{}, format{1}, width{16}, height{21}, stride{64};
    double scaleX{1}, scaleY{1}, offsetX{}, offsetY{};
    std::array<DWORD, 16 * 70> pixels{};
    BYTE* alternateData{};
    unsigned flushes{}, dataReads{}, marks{}, destroys{};
};

enum class Failure { None, NoAllocation, SourceAlias, ErrorSurface, WrongShape, Flush, Dirty, PixelAlias };
struct CairoFixture {
    Surface source, destination;
    unsigned allocations{};
    Failure failure{};
};
CairoFixture* fixture{};

template<int Surface::*member>
int integer(const void* surface) { return static_cast<const Surface*>(surface)->*member; }

void scale(const void* opaque, double* x, double* y)
{
    const auto& value = *static_cast<const Surface*>(opaque);
    *x = value.scaleX; *y = value.scaleY;
}

void offset(const void* opaque, double* x, double* y)
{
    const auto& value = *static_cast<const Surface*>(opaque);
    *x = value.offsetX; *y = value.offsetY;
}

void flush(void* opaque)
{
    auto& value = *static_cast<Surface*>(opaque);
    ++value.flushes;
    if (fixture->failure == Failure::Flush && &value == &fixture->destination) value.status = 1;
}

void destroy(void* opaque)
{
    auto& value = *static_cast<Surface*>(opaque);
    check(&value == &fixture->destination, "borrowed source must remain owned by Cairo caller");
    ++value.destroys;
}

void dirty(void* opaque)
{
    auto& value = *static_cast<Surface*>(opaque);
    check(&value == &fixture->destination && value.dataReads == 1, "only the painted clone is marked dirty");
    ++value.marks;
    if (fixture->failure == Failure::Dirty) value.status = 1;
}

BYTE* data(void* opaque)
{
    auto& value = *static_cast<Surface*>(opaque);
    check(value.flushes > value.dataReads, "Cairo flush must precede direct data access");
    ++value.dataReads;
    return value.alternateData ? value.alternateData : reinterpret_cast<BYTE*>(value.pixels.data());
}

void* imageCreate(int format, int width, int height)
{
    ++fixture->allocations;
    check(format == 1, "clone uses native RGB24");
    if (fixture->failure == Failure::NoAllocation) return nullptr;
    if (fixture->failure == Failure::SourceAlias) return &fixture->source;
    auto& destination = fixture->destination;
    destination = {};
    destination.width = width;
    destination.height = height;
    destination.stride = width * 4;
    if (fixture->failure == Failure::ErrorSurface) destination.status = 1;
    if (fixture->failure == Failure::WrongShape) ++destination.height;
    if (fixture->failure == Failure::PixelAlias)
        destination.alternateData = reinterpret_cast<BYTE*>(fixture->source.pixels.data());
    return &destination;
}

constexpr CairoApi api{
    integer<&Surface::status>, integer<&Surface::type>, integer<&Surface::format>,
    integer<&Surface::width>, integer<&Surface::height>, integer<&Surface::stride>,
    scale, offset, flush, destroy, dirty, data, imageCreate
};
constexpr Palette palette{0x435465, 0x102030, 0xE0D0C0, 0x385879, 0x7A6452};

void testColors()
{
    CairoFixture f;
    fixture = &f;
    f.source.pixels[0] = 0xFFF0F0F0;
    f.source.pixels[1] = 0xAA787878;
    f.source.pixels[2] = 0x55B4B4B4;
    f.source.pixels[3] = 0xBBD2D2D2;
    const auto stock = f.source.pixels;

    check(clone(api, &f.source, {0, 0, 0, 0}, palette) == &f.destination &&
          f.destination.pixels[0] == palette.arrowBackground && f.destination.pixels[1] == palette.arrowGlyph &&
          f.destination.pixels[2] == 0x787878 && f.destination.pixels[3] == 0x444C54,
          "RGB24 palette and luminance endpoints with intermediate edge values");
    check(f.destination.width == 16 && f.destination.height == 21 && f.destination.stride == 64 &&
          f.destination.scaleX == 1 && f.destination.offsetY == 0 && f.destination.marks == 1 &&
          !f.destination.destroys && f.source.pixels == stock && !f.source.marks && !f.source.destroys,
          "fresh clone preserves dimensions and borrowed pixels");
    api.destroy(&f.destination);
    check(f.destination.destroys == 1, "caller can release the transferred reference");

    check(clone(api, &f.source, {0, 0, 1, 2}, palette) &&
          f.destination.pixels[0] == palette.arrowHover, "only this arrow's hover ID selects hover");
    check(clone(api, &f.source, {0, 0, 1, 1}, palette) &&
          f.destination.pixels[0] == palette.arrowPressed, "press takes precedence for the same arrow");
    check(clone(api, &f.source, {0, 0, 1, 1, false, true}, palette) &&
          f.destination.pixels[0] == palette.arrowBackground && f.destination.pixels[3] == 0x444C54,
          "disabled arrow keeps default background and source contrast");
    check(clone(api, &f.source, {0, 0, 1, 0, true, false}, palette) &&
          f.destination.pixels[0] == palette.arrowBackground, "noninteractive arrow ignores hover ID");
    f.source.width = 21; f.source.height = 16; f.source.stride = 84;
    check(clone(api, &f.source, {1, 1, 1, 2}, palette) &&
          f.destination.pixels[0] == palette.arrowPressed && f.destination.width == 21 &&
          f.destination.height == 16 && f.destination.stride == 84, "other end and horizontal arrow select its own ID");
    check(clone(api, &f.source, {1, 1, 1, 1}, palette) &&
          f.destination.pixels[0] == palette.arrowBackground, "other end ignores foreign IDs");

    for (DWORD axis = 0; axis < 2; ++axis) {
        f.source.width = axis ? 70 : 16;
        f.source.height = axis ? 16 : 70;
        f.source.stride = f.source.width * 4;
        const auto reads = f.source.dataReads;
        check(clone(api, &f.source, {3, axis, 5, 4}, palette) &&
              f.destination.pixels.front() == palette.trackBackground &&
              f.destination.pixels.back() == palette.trackBackground && f.source.dataReads == reads &&
              f.source.pixels == stock, "solid track uses separate palette color without reading stock pixels");
    }
}

void testFallbacks()
{
    CairoFixture f;
    fixture = &f;
    constexpr Identity arrow{0, 0, 0, 0};
    check(!clone(api, nullptr, arrow, palette) && !clone(api, &f.source, {2, 0, 0, 0}, palette) &&
          !clone(api, &f.source, {0, 2, 0, 0}, palette) &&
          !clone(api, &f.source, {0, 0, 6, 0}, palette) && !f.allocations, "bad input cannot allocate");
    for (const auto field : {&Surface::status, &Surface::type, &Surface::format,
                             &Surface::width, &Surface::height, &Surface::stride}) {
        f.source = {};
        ++(f.source.*field);
        check(!clone(api, &f.source, arrow, palette) && !f.allocations && !f.source.flushes,
              "unconfirmed source status, format and dimensions rejected before pixel access");
    }
    for (const auto field : {&Surface::scaleX, &Surface::scaleY, &Surface::offsetX, &Surface::offsetY}) {
        f.source = {};
        ++(f.source.*field);
        check(!clone(api, &f.source, arrow, palette) && !f.allocations, "unconfirmed transform rejected");
    }
    f.source = {};
    f.source.pixels[0] = 0xFFFF0000;
    const auto stock = f.source.pixels;
    for (const auto failure : {Failure::NoAllocation, Failure::SourceAlias, Failure::ErrorSurface,
                               Failure::WrongShape, Failure::Flush, Failure::Dirty, Failure::PixelAlias}) {
        f.destination = {};
        f.failure = failure;
        const bool owned = failure != Failure::NoAllocation && failure != Failure::SourceAlias;
        check(!clone(api, &f.source, arrow, palette) && f.destination.destroys == (owned ? 1U : 0U) &&
              !f.source.destroys && !f.source.marks && f.source.pixels == stock,
              "on failure release only the owned clone and preserve source");
    }
    f.failure = Failure::None;
    const auto guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    check(guard != nullptr, "allocate inaccessible pixel fixture");
    f.source.alternateData = static_cast<BYTE*>(guard);
    check(!clone(api, &f.source, arrow, palette) && f.destination.destroys == 1 && !f.destination.marks,
          "guarded pixel data declines clone");
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(guard, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "guard remains set");
    check(VirtualFree(guard, 0, MEM_RELEASE) != FALSE, "release guarded fixture");
}
} // namespace

int main()
{
    testMatcher();
    testArrowFlags();
    testColors();
    testFallbacks();
    std::puts("PASS: scrollbar PNG routes, bounded RGB24 color copies, state palette and safe fallback");
}
