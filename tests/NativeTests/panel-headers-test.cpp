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
    Region image{0xB25000}, owner, bar, item;
    std::array<ULONG_PTR, 0x500 / sizeof(ULONG_PTR)> stack{};
    ULONG_PTR frame = image.address();

    Fixture()
    {
        bar.put(0, frame + 0xB23EA8); bar.put(0x58, frame + 0xB23578);
        bar.put(0x678, owner.address()); owner.put(0xD0, bar.address());
        item.put(0, frame + 0xAFDE70); item.put(0x20, frame + 0xAFE098);
        item.put(0x28, frame + 0xAFE110);
        image.put(0xAFE098 + 0x18, frame + 0x73B3E0);
    }

    CONTEXT context(panel_detail::Paint paint, bool mdiCaption = false)
    {
        using panel_detail::Paint;
        stack.fill(0);
        owner.put(0, frame + (paint == Paint::MdiFrame || mdiCaption ? 0xB224A8 : 0xB22620));
        CONTEXT c{};
        c.Rsp = reinterpret_cast<ULONG_PTR>(stack.data()); c.R8 = 0xB2B2B2;
        c.Rcx = 0xCAFE; c.Rdx = 0xBEEF; c.R14 = 0x1414; c.R12 = 0x1212;
        c.EFlags = 0x202; c.Rax = 0x123456789ABCDEF0; c.R9 = 0x9999;
        c.Rip = frame + panel_detail::patches[static_cast<size_t>(paint)].rva + 1;
        if (paint == Paint::Caption) {
            stack[0x1C8 / 8] = frame + 0x73B4A7; stack[0x288 / 8] = frame + 0x712498;
            stack[0x338 / 8] = frame + 0x70FC6C; stack[0x438 / 8] = frame + 0x70FB75;
            // 733870: 8 pushes + 188h. 73B410: push + B0h.
            // The saves at 73B424/428/430 are relative to wrapper entry RSP.
            constexpr auto wrapperEntry = 8 * 8 + 0x188 + 8 + 8 + 0xB0;
            static_assert(wrapperEntry == 0x288);
            stack[(wrapperEntry + 8) / 8] = item.address() + 0x28;
            stack[(wrapperEntry + 0x10) / 8] = c.Rsp + 0x310;
            stack[(wrapperEntry + 0x20) / 8] = bar.address();
            stack[0x2C0 / 8] = item.address() + 0x20;
            stack[0x1D8 / 8] = 0xDEAD; stack[0x1E0 / 8] = 0xBEEF; stack[0x1F0 / 8] = 0xCAFE;
        } else {
            c.Rbx = owner.address(); c.Rsi = paint == Paint::MdiFrame ? owner.address() : 0x5151;
            c.R15 = c.Rsp + 0xC0;
            stack[0x98 / 8] = frame + 0x8B9C41;
            const RECT client{0, 0, 400, 300}, inset{1, 1, 399, 299}, title{1, 1, 399, 18};
            std::memcpy(reinterpret_cast<void*>(c.Rsp + 0xC0), &client, sizeof(client));
            std::memcpy(reinterpret_cast<void*>(c.Rsp + 0x48), &inset, sizeof(inset));
            owner.put(0xF0, title);
            c.Rdx = paint == Paint::DockTitle ? owner.address() + 0xF0 : c.Rsp + 0x48;
        }
        return c;
    }

    CONTEXT buttonContext(ULONG_PTR root, ULONG_PTR method, bool mdiCaption)
    {
        auto c = context(panel_detail::Paint::Caption, mdiCaption);
        stack.fill(0);
        item.put(0, frame + root);
        item.put(0x20, frame + 0xAFE548);
        item.put(0x28, frame + 0xAFE5C0);
        image.put(root + 0x38, frame + method);
        image.put(0xAFE548 + 0x18, frame + 0x73B860);
        // 733870: 8 pushes + 188h. 73B890: 3 pushes + B0h.
        constexpr auto wrapperEntry = 8 * 8 + 0x188 + 8 + 3 * 8 + 0xB0;
        static_assert(wrapperEntry == 0x298);
        stack[0x1C8 / 8] = frame + 0x73B930;
        stack[wrapperEntry / 8] = frame + 0x712498;
        stack[0x348 / 8] = frame + 0x70FC6C;
        stack[0x448 / 8] = frame + 0x70FB75;
        stack[(wrapperEntry - 8) / 8] = bar.address();
        stack[(wrapperEntry + 8) / 8] = item.address() + 0x28;
        stack[(wrapperEntry + 0x10) / 8] = c.Rsp + 0x320;
        stack[0x2D0 / 8] = item.address() + 0x20;
        return c;
    }

    CONTEXT penContext(panel_detail::Paint paint)
    {
        using panel_detail::Paint;
        const bool dock = paint == Paint::DockLightPen || paint == Paint::DockDarkPen;
        auto c = context(dock ? Paint::DockFrame : Paint::MdiFrame);
        c.Rip = frame + panel_detail::patches[static_cast<size_t>(paint)].rva + 1;
        c.Rbp = c.Rsp + (dock ? 0x70 : 0x78);
        c.Rbx = item.address();
        if (dock) stack[0x88 / 8] = owner.address();
        owner.put(0x70, BYTE{0x20});
        owner.put(0xB4, static_cast<DWORD>(c.Rax));
        owner.put(0xE0, RECT{0, 19, 400, 300});
        return c;
    }
};

void replay(CONTEXT c, panel_detail::Paint paint, bool recolor)
{
    const auto index = static_cast<size_t>(paint);
    using panel_detail::Paint;
    CONTEXT expected = c;
    const bool pen = paint >= Paint::MdiLightPen;
    if (pen) {
        expected.Rdx = c.Rbp - (paint == Paint::DockLightPen || paint == Paint::DockDarkPen ? 0x38 : 0x40);
        if (recolor) expected.Rax = toBgr(0x3A4B5C);
    } else {
        if (paint == Paint::Caption) expected.Rdx = c.R12;
        else expected.Rcx = paint == Paint::MdiFrame ? c.R14 : c.Rsi;
        if (recolor) expected.R8 = toBgr(paint == Paint::Caption ? 0xA1B2C3 : 0x102030);
    }
    expected.Rip = reinterpret_cast<ULONG_PTR>(sites[dockFrameFill + index].address) + (pen ? 4 : 3);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = sites[dockFrameFill + index].address;
    EXCEPTION_POINTERS exception{&record, &c};
    SetLastError(0x1551);
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION, "own site handled");
    check(GetLastError() == 0x1551, "LastError preserved");
    check(std::memcmp(&c, &expected, sizeof(c)) == 0, "only instruction replay, RIP and accepted color change");
}

void exercisePen(Fixture& f, ThemeProfile& profile, panel_detail::Paint paint)
{
    using panel_detail::Paint;
    const auto c = f.penContext(paint);
    replay(c, paint, true);
    profile.zones = enableToolbarBorder; replay(c, paint, false); profile.zones = enablePanelHeaders;

    const auto& site = sites[dockFrameFill + static_cast<size_t>(paint)];
    check(site.length == 4 && std::memcmp(site.address, site.instruction.data(), 4) == 0,
        "pen site expected four-byte LEA");
    site.address[15] ^= 0xFF;
    check(!panelPatchSignaturesMatch(), "reject mismatch in final signature byte");
    site.address[15] ^= 0xFF;

    const bool dock = paint == Paint::DockLightPen || paint == Paint::DockDarkPen;
    auto wrong = c;
    if (dock) f.stack[0x88 / 8] = f.item.address();
    else wrong.Rsi = f.item.address();
    replay(wrong, paint, false);
    (void)f.penContext(paint);
    f.owner.put(0, f.frame + 0xA8A278); replay(c, paint, false);
    f.owner.put(0, f.frame + (dock ? 0xB224A8 : 0xB22620)); replay(c, paint, false);
    (void)f.penContext(paint);
    f.bar.put(0x678, f.item.address()); replay(c, paint, false);
    f.bar.put(0x678, f.owner.address());
    f.owner.put(0xD0, f.item.address()); replay(c, paint, false);
    f.owner.put(0xD0, f.bar.address());
    f.owner.put(0x70, BYTE{0x10}); replay(c, paint, false);
    f.owner.put(0x70, BYTE{0x20});

    DWORD old{};
    check(VirtualProtect(f.owner.data, 4096, PAGE_READWRITE | PAGE_GUARD, &old), "set pen owner guard");
    replay(c, paint, false);
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(f.owner.data, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
        "pen owner guard rejected without consuming it");
    check(VirtualProtect(f.owner.data, 4096, old, &old), "restore pen owner guard");

    f.stack[0x98 / 8] = f.frame + 0x8B9C72; replay(c, paint, false);
    (void)f.penContext(paint);
    wrong = c; ++wrong.Rbp; replay(wrong, paint, false);
    wrong = c; wrong.R15 += 4; replay(wrong, paint, false);
    wrong = c; wrong.Rsp = wrong.Rbp = (std::numeric_limits<ULONG_PTR>::max)() - 16;
    replay(wrong, paint, false);
    wrong = c; wrong.Rsp = f.item.address(); wrong.Rbp = wrong.Rsp + (dock ? 0x70 : 0x78);
    wrong.R15 = wrong.Rsp + 0xC0; replay(wrong, paint, false);
    const RECT empty{0, 0, 0, 300}, malformed{1, 1, 400, 300};
    std::memcpy(reinterpret_cast<void*>(c.Rsp + 0xC0), &empty, sizeof(empty));
    replay(c, paint, false);
    (void)f.penContext(paint);
    std::memcpy(reinterpret_cast<void*>(c.Rsp + 0x48), &malformed, sizeof(malformed));
    replay(c, paint, false);

    if (paint == Paint::MdiInnerPen) {
        (void)f.penContext(paint);
        f.owner.put(0xB4, DWORD{0}); replay(c, paint, false);
        f.owner.put(0xB4, static_cast<DWORD>(c.Rax));
        f.owner.put(0xE0, empty); replay(c, paint, false);
        f.owner.put(0xE0, RECT{0, 19, (std::numeric_limits<LONG>::max)(), 300}); replay(c, paint, false);
    }
}

void exercise()
{
    using panel_detail::Paint;
    Fixture f;
    frameBase = f.frame;
    for (size_t i = 0; i < panel_detail::patches.size(); ++i) {
        const auto& patch = panel_detail::patches[i];
        auto* const address = f.image.data + patch.rva;
        std::memcpy(address, patch.bytes.data(), patch.bytes.size());
        sites[dockFrameFill + i].address = address;
    }
    check(panelPatchSignaturesMatch(), "all panel signatures accepted");
    ThemeProfile profile{};
    profile.zones = enablePanelHeaders;
    profile.colors[panelHeaderBackground] = 0x102030;
    profile.colors[panelHeaderText] = 0xA1B2C3;
    profile.colors[panelBorder] = 0x3A4B5C;
    activeProfile.store(&profile);
    for (const auto paint : {Paint::DockFrame, Paint::MdiFrame, Paint::DockTitle, Paint::Caption}) {
        auto c = f.context(paint);
        replay(c, paint, true);
        profile.zones = 0; replay(c, paint, false); profile.zones = enablePanelHeaders;
        f.owner.put(0, f.frame + 0xA8A278); replay(c, paint, false); // unrelated grid/editor/input family
        c = f.context(paint);
        f.bar.put(0x678, f.item.address()); replay(c, paint, false);
        f.bar.put(0x678, f.owner.address());
        f.owner.put(0xD0, f.item.address()); replay(c, paint, false);
        f.owner.put(0xD0, f.bar.address());
        DWORD old{};
        check(VirtualProtect(f.bar.data, 4096, PAGE_READWRITE | PAGE_GUARD, &old), "set guard");
        replay(c, paint, false);
        MEMORY_BASIC_INFORMATION region{};
        check(VirtualQuery(f.bar.data, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
              "guard rejected without consuming it");
        check(VirtualProtect(f.bar.data, 4096, old, &old), "restore guard");
    }
    auto caption = f.context(Paint::Caption, true);
    replay(caption, Paint::Caption, true);
    for (const auto slot : {0x1C8, 0x288, 0x338, 0x438, 0x290, 0x298, 0x2A8, 0x2C0}) {
        const auto old = f.stack[slot / 8]; f.stack[slot / 8] = 0;
        replay(caption, Paint::Caption, false); f.stack[slot / 8] = old;
    }
    for (const auto foreign : {0xAFE7E0u, 0xAFE5C0u, 0xAF81F8u}) {
        f.item.put(0x28, f.frame + foreign); replay(caption, Paint::Caption, false);
    }
    f.item.put(0x28, f.frame + 0xAFE110);
    f.image.put(0xAFE098 + 0x18, f.frame + 0x73B860); replay(caption, Paint::Caption, false);
    f.image.put(0xAFE098 + 0x18, f.frame + 0x73B3E0);
    for (bool mdi : {false, true}) {
        replay(f.buttonContext(0xAFEB18, 0x73BC00, mdi), Paint::Caption, true);
        replay(f.buttonContext(0xAFE8D0, 0x73BDC0, mdi), Paint::Caption, true);
        replay(f.buttonContext(0xAFEA18, 0x73C0B0, mdi), Paint::Caption, true);
        replay(f.buttonContext(0xAFE998, 0x73C250, mdi), Paint::Caption, true);
    }
    auto button = f.buttonContext(0xAFEB18, 0x73BC00, true);
    profile.zones = 0; replay(button, Paint::Caption, false); profile.zones = enablePanelHeaders;
    for (const auto slot : {0x1C8, 0x298, 0x348, 0x448, 0x290, 0x2A0, 0x2A8, 0x2D0}) {
        const auto old = f.stack[slot / 8]; f.stack[slot / 8] = 0;
        replay(button, Paint::Caption, false); f.stack[slot / 8] = old;
    }
    f.stack[0x2D0 / 8] = f.item.address() + 0x28; replay(button, Paint::Caption, false);
    f.stack[0x2D0 / 8] = f.item.address() + 0x20;
    f.image.put(0xAFEB18 + 0x38, f.frame + 0x73C250); replay(button, Paint::Caption, false);
    button = f.buttonContext(0xAFEB18, 0x73BC00, false);
    f.image.put(0xAFE548 + 0x18, f.frame + 0x73B3E0); replay(button, Paint::Caption, false);
    button = f.buttonContext(0xAFEB18, 0x73BC00, false);
    f.owner.put(0, f.frame + 0xA8A278); replay(button, Paint::Caption, false);
    button = f.buttonContext(0xAFEB18, 0x73BC00, true);
    f.owner.put(0xD0, f.item.address()); replay(button, Paint::Caption, false);
    f.owner.put(0xD0, f.bar.address());
    f.bar.put(0x678, f.item.address()); replay(button, Paint::Caption, false);
    f.bar.put(0x678, f.owner.address());
    f.item.put(0, f.frame + 0xAFE368); replay(button, Paint::Caption, false);
    f.item.put(0, f.frame + 0xAFEB18);
    f.item.put(0x28, f.frame + 0xAFE7E0); replay(button, Paint::Caption, false);
    f.item.put(0x28, f.frame + 0xAFE5C0);
    f.item.put(0x20, f.frame + 0xAFE508); replay(button, Paint::Caption, false);
    f.item.put(0, f.frame + 0xAFE368);
    f.item.put(0x28, f.frame + 0xAFE7E0);
    f.image.put(0xAFE508 + 0x18, f.frame + 0x73B860);
    replay(button, Paint::Caption, false); // complete picture renderer, same wrapper
    auto fill = f.context(Paint::DockFrame);
    f.stack[0x98 / 8] = 0; replay(fill, Paint::DockFrame, false);
    fill = f.context(Paint::DockFrame); fill.Rdx += 4; replay(fill, Paint::DockFrame, false);
    fill = f.context(Paint::DockFrame);
    const RECT malformed{1, 1, 400, 300};
    std::memcpy(reinterpret_cast<void*>(fill.Rsp + 0x48), &malformed, sizeof(malformed));
    replay(fill, Paint::DockFrame, false);
    auto title = f.context(Paint::DockTitle);
    f.owner.put(0xF0, RECT{-1, 0, 400, 300}); replay(title, Paint::DockTitle, false);
    title = f.context(Paint::DockTitle); title.Rsp = (std::numeric_limits<ULONG_PTR>::max)() - 16;
    replay(title, Paint::DockTitle, false);
    for (const auto paint : {Paint::MdiLightPen, Paint::MdiDarkPen, Paint::MdiInnerPen,
                            Paint::DockLightPen, Paint::DockDarkPen})
        exercisePen(f, profile, paint);
    activeProfile.store(&disabledProfile);
}
}

int main()
{
    exercise();
    std::puts("PASS: local frame fills, caption and glyph colors, scoped frame pens, signatures and replay");
}
