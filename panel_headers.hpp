#pragma once

#include "platform.hpp"
#include <cstring>

// Only the hash-gated 8.3.27.2325 layout. The first two fills are the local
// frame's interior, not the content window or a caption-height rectangle.
namespace panel_detail {
enum class Paint : size_t { DockFrame, MdiFrame, DockTitle, Caption };
struct Patch { ULONG_PTR rva; std::array<BYTE, 16> bytes; };
inline constexpr std::array<Patch, 4> patches{{
    {0x8BC4BE, {0x48,0x8B,0xCE,0xFF,0x15,0xA1,0x80,0x16,0x00,0x41,0x8D,0x54,0x24,0x19,0x44,0x89}},
    {0x8BAACE, {0x49,0x8B,0xCE,0xFF,0x15,0x91,0x9A,0x16,0x00,0xBA,0x19,0x00,0x00,0x00,0x44,0x89}},
    {0x8BC71D, {0x48,0x8B,0xCE,0xFF,0x15,0x42,0x7E,0x16,0x00,0xEB,0xC2,0xCC,0xCC,0xCC,0xCC,0xCC}},
    {0x733E09, {0x49,0x8B,0xD4,0x41,0xFF,0xD1,0x89,0x44,0x24,0x7C,0x48,0x8B,0x0D,0x86,0x4E,0x69}}
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
    if (!frame || frame > limit - 0xB23F20) return false;
    ULONG_PTR actual{}, side{}, back{};
    return r.read(owner, 0, actual) && actual == frame + table &&
        r.read(owner, 0xD0, bar) && r.read(bar, 0, actual) && actual == frame + 0xB23F20 &&
        r.read(bar, 0x58, side) && side == frame + 0xB23848 &&
        r.read(bar, 0x678, back) && back == owner;
}

inline bool nonempty(const RECT& rect)
{ return rect.left < rect.right && rect.top < rect.bottom; }

inline bool fillMatches(const CONTEXT& c, ULONG_PTR frame, Paint paint)
{
    Reader r;
    ULONG_PTR caller{}, bar{}, originalAddress{};
    RECT original{}, destination{};
    const auto owner = paint == Paint::MdiFrame ? c.Rsi : c.Rbx;
    const auto table = paint == Paint::MdiFrame ? 0xB22710u : 0xB22788u;
    if (!ownerMatches(r, owner, frame, table, bar) ||
        !r.stack(c.Rsp, 0x98, caller) || caller != frame + 0x8B9C71 ||
        c.Rsp > limit - 0xC0 || c.R15 != c.Rsp + 0xC0 ||
        !r.stack(c.Rsp, 0xC0, original) || !nonempty(original)) return false;
    if (paint == Paint::DockTitle) {
        if (owner > limit - 0xF0 || c.Rdx != owner + 0xF0 ||
            !r.read(owner, 0xF0, destination) || !nonempty(destination)) return false;
        return destination.left >= original.left && destination.top >= original.top &&
            destination.right <= original.right && destination.bottom <= original.bottom;
    }
    originalAddress = c.Rsp + 0x48;
    if (c.Rdx != originalAddress || !r.stack(c.Rsp, 0x48, destination) || !nonempty(destination))
        return false;
    // Use wide arithmetic: malformed LONG edges must not overflow a test.
    return static_cast<LONGLONG>(destination.left) == static_cast<LONGLONG>(original.left) + 1 &&
        static_cast<LONGLONG>(destination.top) == static_cast<LONGLONG>(original.top) + 1 &&
        static_cast<LONGLONG>(destination.right) == static_cast<LONGLONG>(original.right) - 1 &&
        static_cast<LONGLONG>(destination.bottom) == static_cast<LONGLONG>(original.bottom) - 1;
}

inline bool localBarMatches(Reader& r, ULONG_PTR bar, ULONG_PTR frame)
{
    ULONG_PTR owner{}, table{}, checkedBar{};
    if (!r.read(bar, 0x678, owner) || !r.read(owner, 0, table) ||
        (table != frame + 0xB22788 && table != frame + 0xB22710)) return false;
    return ownerMatches(r, owner, frame, table - frame, checkedBar) && checkedBar == bar;
}

inline bool captionMatches(const CONTEXT& c, ULONG_PTR frame)
{
    if (!frame || frame > limit - 0xB23F20) return false;
    Reader r;
    constexpr std::array<ULONG_PTR, 4> slots{0x1C8, 0x288, 0x338, 0x438};
    constexpr std::array<ULONG_PTR, 4> returns{0x73B4D7, 0x7124C8, 0x70FC9C, 0x70FBA5};
    ULONG_PTR value{}, raw{}, savedRbp{}, bar{}, draw{};
    for (size_t i = 0; i < slots.size(); ++i)
        if (!r.stack(c.Rsp, slots[i], value) || value != frame + returns[i]) return false;
    // 73B410 saves these registers at its ENTRY RSP + 8/10h/20h.
    // Its outgoing call-home area is not the saved-register area.
    if (!r.stack(c.Rsp, 0x290, raw) || raw < 0x28 ||
        !r.stack(c.Rsp, 0x298, savedRbp) || c.Rsp > limit - 0x310 || savedRbp != c.Rsp + 0x310 ||
        !r.stack(c.Rsp, 0x2C0, draw) || draw != raw - 8 ||
        !r.read(raw - 0x28, 0, value) || value != frame + 0xAFE280 ||
        !r.read(raw, 0, value) || value != frame + 0xAFE100 ||
        !r.read(draw, 0, value) || value != frame + 0xAFE088 ||
        !r.read(value, 0x18, value) || value != frame + 0x73B410 ||
        !r.stack(c.Rsp, 0x2A8, bar)) return false;
    return localBarMatches(r, bar, frame);
}

inline bool buttonMatches(const CONTEXT& c, ULONG_PTR frame)
{
    if (!frame || frame > limit - 0xB23F20) return false;
    Reader r;
    constexpr std::array<ULONG_PTR, 4> slots{0x1C8, 0x298, 0x348, 0x448};
    constexpr std::array<ULONG_PTR, 4> returns{0x73B960, 0x7124C8, 0x70FC9C, 0x70FBA5};
    ULONG_PTR value{}, raw{}, savedRbp{}, bar{}, draw{}, root{}, method{};
    for (size_t i = 0; i < slots.size(); ++i)
        if (!r.stack(c.Rsp, slots[i], value) || value != frame + returns[i]) return false;
    // 73B890 pushes the caller's RDI (bar), and saves RBX (raw) and RBP
    // at entry RSP + 8/10h. 712330 keeps the queried draw interface on its stack.
    if (!r.stack(c.Rsp, 0x290, bar) ||
        !r.stack(c.Rsp, 0x2A0, raw) || raw < 0x28 ||
        !r.stack(c.Rsp, 0x2A8, savedRbp) || c.Rsp > limit - 0x320 || savedRbp != c.Rsp + 0x320 ||
        !r.stack(c.Rsp, 0x2D0, draw) || draw != raw - 8 ||
        !r.read(raw, 0, value) || value != frame + 0xAFDE60 ||
        !r.read(draw, 0, value) || value != frame + 0xAFE768 ||
        !r.read(value, 0x18, value) || value != frame + 0x73B890 ||
        !r.read(raw - 0x28, 0, root)) return false;
    // These four renderer methods consume VDC+74h set by this call site.
    // The picture renderer shares the wrapper but has different interfaces.
    constexpr std::array<ULONG_PTR, 4> roots{0xAFEB48, 0xAFE950, 0xAFEA50, 0xAFE9D0};
    constexpr std::array<ULONG_PTR, 4> methods{0x73BC30, 0x73BDF0, 0x73C0E0, 0x73C280};
    for (size_t i = 0; i < roots.size(); ++i)
        if (root == frame + roots[i])
            return r.read(root, 0x38, method) && method == frame + methods[i] &&
                localBarMatches(r, bar, frame);
    return false;
}

inline void apply(CONTEXT& c, ULONG_PTR frame, Paint paint, const ThemeProfile& profile)
{
    // Replay the displaced MOV even when disabled or when classification rejects.
    if (paint == Paint::Caption) c.Rdx = c.R12;
    else c.Rcx = paint == Paint::MdiFrame ? c.R14 : c.Rsi;
    if (!(profile.zones & enablePanelHeaders)) return;
    const bool matches = paint == Paint::Caption ? captionMatches(c, frame) || buttonMatches(c, frame) :
        fillMatches(c, frame, paint);
    if (matches) c.R8 = toBgr(profile.colors[paint == Paint::Caption ? panelHeaderText : panelHeaderBackground]);
}
} // namespace panel_detail
