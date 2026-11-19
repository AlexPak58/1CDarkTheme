#include "theme.cpp"
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok, const char* why)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

struct Region {
    BYTE* data;
    explicit Region(size_t size = 4096) : data(static_cast<BYTE*>(VirtualAlloc(nullptr, size,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) { check(data != nullptr, "allocate fixture"); }
    ~Region() { VirtualFree(data, 0, MEM_RELEASE); }
    ULONG_PTR address() const { return reinterpret_cast<ULONG_PTR>(data); }
    template<class T> void put(size_t offset, T value) { std::memcpy(data + offset, &value, sizeof(value)); }
};

struct Fixture {
    Region frame{0xB30000}, graphics, root, dc, brush;
    std::array<ULONG_PTR, 0x600 / sizeof(ULONG_PTR)> stack{};
    template<class T> void put(size_t offset, T value)
    { std::memcpy(reinterpret_cast<BYTE*>(stack.data()) + offset, &value, sizeof(value)); }
    CONTEXT context(RECT full = {448, 0, 452, 679}, RECT clip = {448, 0, 452, 679})
    {
        stack.fill(0);
        root.put(0, frame.address() + 0xB2FA68);
        root.put(0xB0, frame.address() + 0xB2FE10);
        root.put(0x188, frame.address() + 0xB2FD50);
        frame.put(0xB2FD50 + 0x18, frame.address() + 0x92AF00);
        root.put(0x208, full); root.put(0x268, barBackgroundBgr);
        // Deliberately not a pointer: graphics brushes are opaque pool handles.
        root.put(0x270, ULONG_PTR{0xC001}); dc.put(0x90, ULONG_PTR{0xC001});
        dc.put(0x98, brush.address()); dc.put(0xD9, BYTE{0});
        put(0x98, graphics.address() + 0xABE0C); put(0xE8, graphics.address() + 0xB3932);
        put(0x148, frame.address() + 0x92AFB9); put(0x1D8, frame.address() + 0x8E9FB7);
        put(0x1C8, root.address()); put(0x160, ULONG_PTR{0xDCDC}); put(0x1D0, ULONG_PTR{0xDCDC});
        put(0x158, graphics.address() + 0xECA60); put(0x190, clip);
        put(0x170, static_cast<DWORD>(static_cast<LONGLONG>(clip.right) - clip.left));
        put(0x178, static_cast<DWORD>(static_cast<LONGLONG>(clip.bottom) - clip.top));
        put(0x180, DWORD{0x00F00021});
        CONTEXT c{};
        c.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
        c.Rdi = dc.address(); c.Rbx = brush.address(); c.EFlags = 0x202;
        c.Rax = 0xCAFE000000000000 | barBackgroundBgr; c.Rdx = 0xFFFF0000AABBCCDD;
        c.Rcx = 0x1234; c.R8 = 0x8888; c.R9 = 0x9999; c.Rbp = 0xBBBB;
        c.Rip = reinterpret_cast<ULONG_PTR>(sites[emptyFill].address) + 1;
        return c;
    }
};

void paint(CONTEXT c, DWORD expectedColor)
{
    CONTEXT expected = c;
    expected.Rdx = expectedColor;
    expected.Rip = reinterpret_cast<ULONG_PTR>(sites[emptyFill].address) + 3;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT; record.ExceptionAddress = sites[emptyFill].address;
    EXCEPTION_POINTERS exception{&record, &c};
    SetLastError(0x1551);
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION, "handled own breakpoint");
    check(GetLastError() == 0x1551, "preserved LastError");
    check(!std::memcmp(&c, &expected, sizeof(c)), "only replay, RIP and intended color change");
}

void exercise()
{
    Fixture f;
    frameBase = f.frame.address(); graphicsBase = f.graphics.address();
    for (size_t i = 0; i < std::size(sites); ++i)
        sites[i].address = reinterpret_cast<BYTE*>(ULONG_PTR{0x5000} + 0x10 * i);
    ThemeProfile profile;
    profile.zones = enableDockSplitter; profile.colors[dockSplitterFill] = 0x123456;
    activeProfile.store(&profile);
    const DWORD changed = toBgr(profile.colors[dockSplitterFill]);
    auto c = f.context(); paint(c, changed);
    paint(f.context({0, 0, 1920, 4}, {0, 0, 1920, 4}), changed);
    paint(f.context({448, 0, 452, 679}, {449, 50, 452, 120}), changed);
    // PATCOPY rejects non-positive signed dimensions before this call chain.
    paint(f.context({LONG_MIN, 0, -1, 4}, {LONG_MIN, 0, -1, 4}), changed);
    paint(f.context({LONG_MIN, 0, LONG_MAX, 4}, {LONG_MIN, 0, LONG_MAX, 4}), barBackgroundBgr);
    paint(f.context({0, LONG_MIN, 4, LONG_MAX}, {0, LONG_MIN, 4, LONG_MAX}), barBackgroundBgr);
    c = f.context(); profile.zones = 0; paint(c, barBackgroundBgr); profile.zones = enableDockSplitter;
    c.EFlags |= 0x40; paint(c, static_cast<DWORD>(c.Rdx));
    c = f.context(); c.Rax = 0x00123456; paint(c, 0x00123456);
    for (size_t slot : {0x98, 0xE8, 0x148, 0x1D8, 0x1C8, 0x160, 0x1D0, 0x158}) {
        c = f.context(); f.put(slot, ULONG_PTR{0}); paint(c, barBackgroundBgr);
    }
    for (size_t field : {0, 0xB0, 0x188}) {
        c = f.context(); f.root.put(field, ULONG_PTR{0}); paint(c, barBackgroundBgr);
    }
    c = f.context(); f.root.put(0, frameBase + 0xB31DB8); paint(c, barBackgroundBgr);
    c = f.context(); f.frame.put(0xB2FD50 + 0x18, frameBase + 0x9320E0); paint(c, barBackgroundBgr);
    for (ULONG_PTR caller : {0x932190, 0x932276, 0x8E9CF7, 0xA4638}) {
        c = f.context(); f.put(0x148, frameBase + caller); paint(c, barBackgroundBgr);
    }
    for (RECT clip : {RECT{448, 0, 448, 679}, RECT{447, 0, 452, 679},
                     RECT{448, -1, 452, 679}, RECT{448, 0, 453, 679}, RECT{448, 0, 452, 680}}) {
        c = f.context(); f.put(0x190, clip); paint(c, barBackgroundBgr);
    }
    for (size_t slot : {0x170, 0x178, 0x180}) {
        c = f.context(); f.put(slot, DWORD{0}); paint(c, barBackgroundBgr);
    }
    c = f.context(); f.root.put(0x268, DWORD{0}); paint(c, barBackgroundBgr);
    c = f.context(); f.root.put(0x270, ULONG_PTR{0}); paint(c, barBackgroundBgr);
    for (size_t field : {0x90, 0x98}) {
        c = f.context(); f.dc.put(field, ULONG_PTR{0}); paint(c, barBackgroundBgr);
    }
    c = f.context(); f.dc.put(0xD9, BYTE{1}); paint(c, barBackgroundBgr);
    c = f.context(); c.Rbx = 0; paint(c, barBackgroundBgr);
    c = f.context(); c.Rdi = 1; paint(c, barBackgroundBgr);
    c = f.context(); f.put(0x1C8, panel_detail::limit - 7); paint(c, barBackgroundBgr);
    c = f.context(); c.Rsp = panel_detail::limit - 7; paint(c, barBackgroundBgr);
    c = f.context();
    check(!dock_detail::matches(c, panel_detail::limit, graphicsBase) &&
          !dock_detail::matches(c, frameBase, panel_detail::limit), "module arithmetic overflow refused");
    for (Region* region : {&f.root, &f.dc}) {
        c = f.context(); DWORD old{};
        check(VirtualProtect(region->data, 4096, PAGE_READWRITE | PAGE_GUARD, &old) != 0, "guard fixture");
        paint(c, barBackgroundBgr);
        MEMORY_BASIC_INFORMATION information{};
        check(VirtualQuery(region->data, &information, sizeof(information)) != 0 &&
              (information.Protect & PAGE_GUARD), "guard not consumed");
        DWORD ignored{}; check(VirtualProtect(region->data, 4096, old, &ignored) != 0, "restore guard");
        check(VirtualProtect(region->data, 4096, PAGE_NOACCESS, &old) != 0, "unreadable fixture");
        paint(c, barBackgroundBgr);
        check(VirtualProtect(region->data, 4096, old, &ignored) != 0, "restore unreadable page");
    }
    // A user may choose a stock popup color. A successful dock match is terminal
    // even with an unrelated popup-shaped tail on the stack.
    c = f.context(); profile.zones = themeZoneMask;
    f.put(0x278, frameBase + 0x70FB4B); f.put(0x328, frameBase + 0x50CAE);
    f.put(0x438, frameBase + popupMenuPaintReturn); f.put(0x218, frameBase + 0x7251D5);
    profile.colors[dockSplitterFill] = toBgr(platformPopupMenuBackgroundBgr);
    paint(c, platformPopupMenuBackgroundBgr);
    f.put(0x218, frameBase + 0x725285);
    profile.colors[dockSplitterFill] = toBgr(platformPopupMenuGutterBgr);
    paint(c, platformPopupMenuGutterBgr);
    activeProfile.store(&disabledProfile);
}
}

int main()
{
    exercise();
    std::puts("PASS: dock splitter identity, geometry, brush witnesses, guarded reads, palette isolation and replay");
}
