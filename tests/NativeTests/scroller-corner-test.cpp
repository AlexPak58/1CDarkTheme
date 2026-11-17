#include "theme.cpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
using scroller_corner::Paint;

void check(bool okay, const char* reason)
{
    if (!okay) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}

template<class T, size_t N>
void put(std::array<ULONG_PTR, N>& buffer, size_t offset, T value)
{
    check(offset <= sizeof(buffer) && sizeof(value) <= sizeof(buffer) - offset, "fixture offset");
    std::memcpy(reinterpret_cast<BYTE*>(buffer.data()) + offset, &value, sizeof(value));
}

template<size_t N>
ULONG_PTR address(const std::array<ULONG_PTR, N>& buffer)
{
    return reinterpret_cast<ULONG_PTR>(buffer.data());
}

struct Witnesses {
    std::array<size_t, 5> returns;
    std::array<ULONG_PTR, 5> rvas;
    size_t scroller, layout, slot, offset, rect;
};
constexpr Witnesses fill{{0x98, 0x158, 0x1C8, 0x238, 0x2C8},
    {0xAC84F, 0xB46BF, 0x17466, 0x1E9E0, 0x1D5D9}, 0x1B0, 0x120, 0x240, 0x230, 0x1F8};
constexpr Witnesses stroke{{0x38, 0x78, 0xC8, 0x138, 0x1C8},
    {0xAF1DB, 0xB3121, 0x174D8, 0x1E9E0, 0x1D5D9}, 0xB0, 0x60, 0x140, 0x130, 0xC0};

struct Fixture {
    std::array<ULONG_PTR, 0x500 / 8> stack{};
    std::array<ULONG_PTR, 0x200 / 8> scroller{};
    std::array<ULONG_PTR, 0xC0 / 8> window{};
    std::array<ULONG_PTR, 0xE0 / 8> dc{};
    std::unique_ptr<std::array<ULONG_PTR, 0x40000 / 8>> wbase =
        std::make_unique<std::array<ULONG_PTR, 0x40000 / 8>>();
    ThemeProfile profile;

    Fixture()
    {
        frameBase = 0x10000000;
        graphicsBase = 0x20000000;
        wbaseBase = address(*wbase);
        for (size_t i = 0; i < std::size(sites); ++i)
            sites[i].address = reinterpret_cast<BYTE*>(ULONG_PTR{0x5000} + 0x10 * i);
        profile.zones = enableScrollerCorner;
        profile.colors[scrollerCornerBackground] = 0x123456;
        profile.colors[scrollerCornerBorder] = 0x3579AB;
        activeProfile.store(&profile);
    }

    ~Fixture() { activeProfile.store(&disabledProfile); }

    CONTEXT context(Paint paint, RECT rect = {500, 423, 516, 439})
    {
        stack.fill(0);
        const auto& witnesses = paint == Paint::Fill ? fill : stroke;
        for (size_t i = 0; i < witnesses.returns.size(); ++i)
            put(stack, witnesses.returns[i], (i < 2 ? graphicsBase : wbaseBase) + witnesses.rvas[i]);
        put(stack, witnesses.scroller, address(scroller));
        put(stack, witnesses.layout, address(scroller) + 0xA8);
        put(stack, witnesses.slot, address(scroller) + 0xE0);
        put(stack, witnesses.offset, ULONG_PTR{0x70});
        put(stack, witnesses.rect, address(scroller) + 0x118);
        put(scroller, 0, wbaseBase + 0x3CA78);
        put(scroller, 0x20, address(window));
        put(scroller, 0xE0, address(scroller) + 0x1C8);
        put(scroller, 0x118, rect);
        put(scroller, 0x1C8, wbaseBase + 0x3CF98);
        put(*wbase, 0x3CA78 + 0xC0, wbaseBase + 0x1E860);
        put(*wbase, 0x3CF98 + 0x10, wbaseBase + 0x17400);
        put(window, 0, frameBase + 0xABEDB0);
        put(window, 0xB8, address(scroller));
        put(dc, 0xD9, BYTE{0});
        CONTEXT c{};
        c.Rsp = address(stack);
        c.Rax = 0x1122334400000000 | scroller_corner::platformFillBgr;
        c.Rdx = 0x55667788AABBCCDD;
        c.Rdi = paint == Paint::Fill ? address(dc) : 0x9988776600000000 | scroller_corner::platformStrokeBgr;
        c.Rbx = paint == Paint::Stroke ? address(dc) : 0x1234;
        c.Rcx = 0x1234;
        c.R8 = 0x5678;
        c.EFlags = 0x202;
        c.Rip = reinterpret_cast<ULONG_PTR>(sites[paint == Paint::Fill ? emptyFill : toolbarEdgeStroke].address) + 1;
        return c;
    }
};

void invoke(Paint paint, const CONTEXT& before, DWORD color)
{
    const bool isFill = paint == Paint::Fill;
    CONTEXT actual = before, expected = before;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = sites[isFill ? emptyFill : toolbarEdgeStroke].address;
    EXCEPTION_POINTERS exception{&record, &actual};
    expected.Rdx = color;
    expected.Rip = reinterpret_cast<ULONG_PTR>(record.ExceptionAddress) + (isFill ? 3 : 2);
    constexpr DWORD lastError = 0x9191;
    SetLastError(lastError);
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION, "handled existing paint breakpoint");
    check(GetLastError() == lastError, "preserved LastError");
    check(!std::memcmp(&actual, &expected, sizeof(actual)), "RDX color and RIP are the only changes");
}

void exercise(Fixture& f, Paint paint)
{
    const bool isFill = paint == Paint::Fill;
    const DWORD stock = isFill ? scroller_corner::platformFillBgr : scroller_corner::platformStrokeBgr;
    const DWORD custom = toBgr(f.profile.colors[isFill ? scrollerCornerBackground : scrollerCornerBorder]);
    const auto& witnesses = isFill ? fill : stroke;
    invoke(paint, f.context(paint), custom);
    invoke(paint, f.context(paint, {-30, 14, -9, 47}), custom);
    f.profile.zones = 0;
    invoke(paint, f.context(paint), stock);
    f.profile.zones = enableScrollerCorner;

    auto c = f.context(paint);
    if (isFill) c.Rax = 0x12345; else c.Rdi = 0x12345;
    invoke(paint, c, 0x12345);
    c = f.context(paint);
    c.EFlags |= 0x40;
    invoke(paint, c, isFill ? static_cast<DWORD>(c.Rdx) : custom);
    for (const auto slot : witnesses.returns) {
        c = f.context(paint);
        ++f.stack[slot / 8];
        invoke(paint, c, stock);
    }
    for (const auto slot : {witnesses.scroller, witnesses.layout, witnesses.slot, witnesses.offset, witnesses.rect}) {
        c = f.context(paint);
        ++f.stack[slot / 8];
        invoke(paint, c, stock);
    }
    for (size_t field : {0, 0x20, 0xE0, 0x1C8}) {
        c = f.context(paint);
        put(f.scroller, field, ULONG_PTR{0});
        invoke(paint, c, stock);
    }
    for (size_t field : {0x3CA78 + 0xC0, 0x3CF98 + 0x10}) {
        c = f.context(paint);
        ++(*f.wbase)[field / 8];
        invoke(paint, c, stock);
    }
    for (size_t field : {0, 0xB8}) {
        c = f.context(paint);
        ++f.window[field / 8];
        invoke(paint, c, stock);
    }
    c = f.context(paint);
    put(f.window, 0, frameBase + 0xA885D0);
    invoke(paint, c, stock);
    invoke(paint, f.context(paint, {8, 5, 8, 7}), stock);
    invoke(paint, f.context(paint, {8, 7, 10, 5}), stock);
    c = f.context(paint);
    put(f.dc, 0xD9, BYTE{1});
    invoke(paint, c, stock);
    c = f.context(paint);
    auto& dcRegister = isFill ? c.Rdi : c.Rbx;
    dcRegister = panel_detail::limit - 7;
    invoke(paint, c, stock);
    c = f.context(paint);
    put(f.stack, witnesses.scroller, panel_detail::limit - 7);
    invoke(paint, c, stock);
    c = f.context(paint);
    c.Rsp = panel_detail::limit - 7;
    invoke(paint, c, stock);
    c = f.context(paint);
    check(!scroller_corner::matches(c, panel_detail::limit, graphicsBase, wbaseBase, paint) &&
          !scroller_corner::matches(c, frameBase, panel_detail::limit, wbaseBase, paint) &&
          !scroller_corner::matches(c, frameBase, graphicsBase, panel_detail::limit, paint), "module base overflow");

    auto* const guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    check(guard != nullptr, "allocate guarded VDC");
    c = f.context(paint);
    dcRegister = reinterpret_cast<ULONG_PTR>(guard);
    invoke(paint, c, stock);
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(guard, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "guard preserved");
    check(VirtualFree(guard, 0, MEM_RELEASE) != FALSE, "release guarded VDC");
}
}

int main()
{
    Fixture fixture;
    exercise(fixture, Paint::Fill);
    exercise(fixture, Paint::Stroke);
    fixture.profile.zones = enableScrollerCorner | enablePopupMenu;
    fixture.profile.colors[scrollerCornerBackground] = toBgr(platformPopupMenuBackgroundBgr);
    const auto context = fixture.context(Paint::Fill);
    put(fixture.stack, 0x218, frameBase + 0x725205);
    put(fixture.stack, 0x278, frameBase + 0x70FB7B);
    put(fixture.stack, 0x328, frameBase + 0x50CAE);
    put(fixture.stack, 0x438, frameBase + popupMenuPaintReturn);
    invoke(Paint::Fill, context, platformPopupMenuBackgroundBgr);
    std::puts("PASS: scrollbar corner fill/stroke identity, palette isolation, guarded reads and instruction replay");
}
