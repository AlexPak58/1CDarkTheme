#pragma once

#include "platform.hpp"
#include <cstring>

// Only the hash-gated 8.3.27.2130 layout. The first two fills are the local
// frame's interior, not the content window or a caption-height rectangle.
namespace panel_detail {
enum class Paint : size_t { DockFrame, MdiFrame, DockTitle, Caption,
    MdiLightPen, MdiDarkPen, MdiInnerPen, DockLightPen, DockDarkPen };
struct Patch { ULONG_PTR rva; std::array<BYTE, 16> bytes; };
inline constexpr std::array<Patch, 9> patches{{
    {0x8BC48E, {0x48,0x8B,0xCE,0xFF,0x15,0xD1,0x80,0x16,0x00,0x41,0x8D,0x54,0x24,0x19,0x44,0x89}},
    {0x8BAA9E, {0x49,0x8B,0xCE,0xFF,0x15,0xC1,0x9A,0x16,0x00,0xBA,0x19,0x00,0x00,0x00,0x44,0x89}},
    {0x8BC6ED, {0x48,0x8B,0xCE,0xFF,0x15,0x72,0x7E,0x16,0x00,0xEB,0xC2,0xCC,0xCC,0xCC,0xCC,0xCC}},
    {0x733DD9, {0x49,0x8B,0xD4,0x41,0xFF,0xD1,0x89,0x44,0x24,0x7C,0x48,0x8B,0x0D,0xB6,0x4E,0x69}},
    {0x8BAAC7, {0x48,0x8D,0x55,0xC0,0x89,0x45,0xCC,0x48,0x8B,0x01,0xFF,0x50,0x18,0x4C,0x8B,0x15}},
    {0x8BABCD, {0x48,0x8D,0x55,0xC0,0x89,0x45,0xCC,0x48,0x8B,0x01,0xFF,0x50,0x18,0x4C,0x8B,0x15}},
    {0x8BACDD, {0x48,0x8D,0x55,0xC0,0x48,0x8B,0x0D,0xB8,0xDF,0x50,0x00,0x89,0x45,0xCC,0x44,0x89}},
    {0x8BC4B7, {0x48,0x8D,0x55,0xC8,0x89,0x45,0xD4,0x48,0x8B,0x01,0xFF,0x50,0x18,0x4C,0x8B,0x15}},
    {0x8BC5BB, {0x48,0x8D,0x55,0xC8,0x89,0x45,0xD4,0x48,0x8B,0x01,0xFF,0x50,0x18,0x4C,0x8B,0x15}}
}};
inline constexpr ULONG_PTR limit = (std::numeric_limits<ULONG_PTR>::max)();

inline bool copy(void* destination, ULONG_PTR address, size_t size)
{
    __try { std::memcpy(destination, reinterpret_cast<const void*>(address), size); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
              GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
              GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

class Reader {
    ULONG_PTR base_{};
    SIZE_T size_{};
    bool contains(ULONG_PTR address, size_t size) const
    { return address >= base_ && address - base_ < size_ && size <= size_ - (address - base_); }
public:
    template<class T> bool read(ULONG_PTR base, ULONG_PTR offset, T& value)
    {
        if (!base || offset > limit - base) return false;
        const auto address = base + offset;
        if (address % alignof(T)) return false;
        if (!contains(address, sizeof(T))) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
                region.State != MEM_COMMIT) return false;
            switch (region.Protect) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
            default: return false;
            }
            base_ = reinterpret_cast<ULONG_PTR>(region.BaseAddress); size_ = region.RegionSize;
            if (!contains(address, sizeof(T))) return false;
        }
        // Per-observation cache only; SEH still covers a protection/lifetime race.
        return copy(&value, address, sizeof(T));
    }
    template<class T> bool stack(ULONG_PTR rsp, ULONG_PTR offset, T& value)
    {
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto low = reinterpret_cast<ULONG_PTR>(tib->StackLimit);
        const auto high = reinterpret_cast<ULONG_PTR>(tib->StackBase);
        if (rsp < low || rsp >= high || offset > high - rsp || sizeof(T) > high - rsp - offset)
            return false;
        return read(rsp, offset, value);
    }
};

inline bool ownerMatches(Reader& r, ULONG_PTR owner, ULONG_PTR frame, ULONG_PTR table, ULONG_PTR& bar)
{
    if (!frame || frame > limit - 0xB23EA8) return false;
    ULONG_PTR actual{}, side{}, back{};
    return r.read(owner, 0, actual) && actual == frame + table &&
        r.read(owner, 0xD0, bar) && r.read(bar, 0, actual) && actual == frame + 0xB23EA8 &&
        r.read(bar, 0x58, side) && side == frame + 0xB23578 &&
        r.read(bar, 0x678, back) && back == owner;
}

inline bool nonempty(const RECT& rect)
{ return rect.left < rect.right && rect.top < rect.bottom; }

inline bool insetMatches(const RECT& original, const RECT& inset)
{
    // Use wide arithmetic: malformed LONG edges must not overflow a test.
    return nonempty(inset) &&
        static_cast<LONGLONG>(inset.left) == static_cast<LONGLONG>(original.left) + 1 &&
        static_cast<LONGLONG>(inset.top) == static_cast<LONGLONG>(original.top) + 1 &&
        static_cast<LONGLONG>(inset.right) == static_cast<LONGLONG>(original.right) - 1 &&
        static_cast<LONGLONG>(inset.bottom) == static_cast<LONGLONG>(original.bottom) - 1;
}

inline bool fillMatches(const CONTEXT& c, ULONG_PTR frame, Paint paint)
{
    Reader r;
    ULONG_PTR caller{}, bar{}, originalAddress{};
    RECT original{}, destination{};
    const auto owner = paint == Paint::MdiFrame ? c.Rsi : c.Rbx;
    const auto table = paint == Paint::MdiFrame ? 0xB224A8u : 0xB22620u;
    if (!ownerMatches(r, owner, frame, table, bar) ||
        !r.stack(c.Rsp, 0x98, caller) || caller != frame + 0x8B9C41 ||
        c.Rsp > limit - 0xC0 || c.R15 != c.Rsp + 0xC0 ||
        !r.stack(c.Rsp, 0xC0, original) || !nonempty(original)) return false;
    if (paint == Paint::DockTitle) {
        if (owner > limit - 0xF0 || c.Rdx != owner + 0xF0 ||
            !r.read(owner, 0xF0, destination) || !nonempty(destination)) return false;
        return destination.left >= original.left && destination.top >= original.top &&
            destination.right <= original.right && destination.bottom <= original.bottom;
    }
    originalAddress = c.Rsp + 0x48;
    return c.Rdx == originalAddress && r.stack(c.Rsp, 0x48, destination) &&
        insetMatches(original, destination);
}

inline bool borderMatches(const CONTEXT& c, ULONG_PTR frame, Paint paint, bool dock)
{
    if (!frame || frame > limit - 0xB23EA8 || c.Rsp > limit - 0xC0 ||
        c.Rbp != c.Rsp + (dock ? 0x70u : 0x78u) || c.R15 != c.Rsp + 0xC0) return false;
    Reader r;
    ULONG_PTR caller{}, owner = c.Rsi, bar{};
    RECT original{}, inset{};
    if (!r.stack(c.Rsp, 0x98, caller) || caller != frame + 0x8B9C41 ||
        !r.stack(c.Rsp, 0xC0, original) || !r.stack(c.Rsp, 0x48, inset) ||
        !insetMatches(original, inset)) return false;
    // Dock's saved RBX is the caller's owner. Live RBX is reused for the pen.
    if (dock && !r.stack(c.Rsp, 0x88, owner)) return false;
    BYTE flags{};
    if (!ownerMatches(r, owner, frame, dock ? 0xB22620u : 0xB224A8u, bar) ||
        !r.read(owner, 0x70, flags) || !(flags & 0x20)) return false;
    if (paint != Paint::MdiInnerPen) return true;
    DWORD color{};
    RECT child{};
    return r.read(owner, 0xB4, color) && color == static_cast<DWORD>(c.Rax) &&
        r.read(owner, 0xE0, child) && nonempty(child) &&
        child.left > (std::numeric_limits<LONG>::min)() &&
        child.top > (std::numeric_limits<LONG>::min)() &&
        child.right < (std::numeric_limits<LONG>::max)() &&
        child.bottom < (std::numeric_limits<LONG>::max)();
}

inline bool localBarMatches(Reader& r, ULONG_PTR bar, ULONG_PTR frame)
{
    ULONG_PTR owner{}, table{}, checkedBar{};
    if (!r.read(bar, 0x678, owner) || !r.read(owner, 0, table) ||
        (table != frame + 0xB22620 && table != frame + 0xB224A8)) return false;
    return ownerMatches(r, owner, frame, table - frame, checkedBar) && checkedBar == bar;
}

inline bool captionMatches(const CONTEXT& c, ULONG_PTR frame)
{
    if (!frame || frame > limit - 0xB23EA8) return false;
    Reader r;
    constexpr std::array<ULONG_PTR, 4> slots{0x1C8, 0x288, 0x338, 0x438};
    constexpr std::array<ULONG_PTR, 4> returns{0x73B4A7, 0x712498, 0x70FC6C, 0x70FB75};
    ULONG_PTR value{}, raw{}, savedRbp{}, bar{}, draw{};
    for (size_t i = 0; i < slots.size(); ++i)
        if (!r.stack(c.Rsp, slots[i], value) || value != frame + returns[i]) return false;
    // 73B410 saves these registers at its ENTRY RSP + 8/10h/20h.
    // Its outgoing call-home area is not the saved-register area.
    if (!r.stack(c.Rsp, 0x290, raw) || raw < 0x28 ||
        !r.stack(c.Rsp, 0x298, savedRbp) || c.Rsp > limit - 0x310 || savedRbp != c.Rsp + 0x310 ||
        !r.stack(c.Rsp, 0x2C0, draw) || draw != raw - 8 ||
        !r.read(raw - 0x28, 0, value) || value != frame + 0xAFDE70 ||
        !r.read(raw, 0, value) || value != frame + 0xAFE110 ||
        !r.read(draw, 0, value) || value != frame + 0xAFE098 ||
        !r.read(value, 0x18, value) || value != frame + 0x73B3E0 ||
        !r.stack(c.Rsp, 0x2A8, bar)) return false;
    return localBarMatches(r, bar, frame);
}

inline bool buttonMatches(const CONTEXT& c, ULONG_PTR frame)
{
    if (!frame || frame > limit - 0xB23EA8) return false;
    Reader r;
    constexpr std::array<ULONG_PTR, 4> slots{0x1C8, 0x298, 0x348, 0x448};
    constexpr std::array<ULONG_PTR, 4> returns{0x73B930, 0x712498, 0x70FC6C, 0x70FB75};
    ULONG_PTR value{}, raw{}, savedRbp{}, bar{}, draw{}, root{}, method{};
    for (size_t i = 0; i < slots.size(); ++i)
        if (!r.stack(c.Rsp, slots[i], value) || value != frame + returns[i]) return false;
    // 73B890 pushes the caller's RDI (bar), and saves RBX (raw) and RBP
    // at entry RSP + 8/10h. 712330 keeps the queried draw interface on its stack.
    if (!r.stack(c.Rsp, 0x290, bar) ||
        !r.stack(c.Rsp, 0x2A0, raw) || raw < 0x28 ||
        !r.stack(c.Rsp, 0x2A8, savedRbp) || c.Rsp > limit - 0x320 || savedRbp != c.Rsp + 0x320 ||
        !r.stack(c.Rsp, 0x2D0, draw) || draw != raw - 8 ||
        !r.read(raw, 0, value) || value != frame + 0xAFE5C0 ||
        !r.read(draw, 0, value) || value != frame + 0xAFE548 ||
        !r.read(value, 0x18, value) || value != frame + 0x73B860 ||
        !r.read(raw - 0x28, 0, root)) return false;
    // These four renderer methods consume VDC+74h set by this call site.
    // The picture renderer shares the wrapper but has different interfaces.
    constexpr std::array<ULONG_PTR, 4> roots{0xAFEB18, 0xAFE8D0, 0xAFEA18, 0xAFE998};
    constexpr std::array<ULONG_PTR, 4> methods{0x73BC00, 0x73BDC0, 0x73C0B0, 0x73C250};
    for (size_t i = 0; i < roots.size(); ++i)
        if (root == frame + roots[i])
            return r.read(root, 0x38, method) && method == frame + methods[i] &&
                localBarMatches(r, bar, frame);
    return false;
}

inline void apply(CONTEXT& c, ULONG_PTR frame, Paint paint, const ThemeProfile& profile)
{
    if (paint >= Paint::MdiLightPen) {
        const bool dock = paint >= Paint::DockLightPen;
        // Replay LEA even for a rejected paint. The original MOV stores EAX next.
        c.Rdx = c.Rbp - (dock ? 0x38u : 0x40u);
        if ((profile.zones & enablePanelHeaders) && borderMatches(c, frame, paint, dock))
            c.Rax = toBgr(profile.colors[panelBorder]);
        return;
    }
    // Replay the displaced MOV even when disabled or when classification rejects.
    if (paint == Paint::Caption) c.Rdx = c.R12;
    else c.Rcx = paint == Paint::MdiFrame ? c.R14 : c.Rsi;
    if (!(profile.zones & enablePanelHeaders)) return;
    const bool matches = paint == Paint::Caption ? captionMatches(c, frame) || buttonMatches(c, frame) :
        fillMatches(c, frame, paint);
    if (matches) c.R8 = toBgr(profile.colors[paint == Paint::Caption ? panelHeaderText : panelHeaderBackground]);
}
} // namespace panel_detail
