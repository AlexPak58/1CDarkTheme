#pragma once

#include "panel_headers.hpp"

namespace scroller_corner {
enum class Paint { Fill, Stroke };

inline constexpr DWORD platformFillBgr = 0x00F3F9FA;
inline constexpr DWORD platformStrokeBgr = 0x00ADC6CC;

// Hash-gated 8.3.27.2325. Both graphics paths return to the same editor corner
// component. The saved registers recover its owner and loop position.
struct Path {
    std::array<ULONG_PTR, 5> returnSlots;
    ULONG_PTR scroller, layout, componentSlot, componentOffset, rect;
};
inline constexpr Path fill{{0x98, 0x158, 0x1C8, 0x238, 0x2C8}, 0x1B0, 0x120, 0x240, 0x230, 0x1F8};
inline constexpr Path stroke{{0x38, 0x78, 0xC8, 0x138, 0x1C8}, 0xB0, 0x60, 0x140, 0x130, 0xC0};

inline bool matches(const CONTEXT& c, ULONG_PTR frame, ULONG_PTR graphics, ULONG_PTR wbase, Paint paint)
{
    using panel_detail::limit;
    if (!frame || frame > limit - 0xABEDB0 || !graphics || graphics > limit - 0xB46BF ||
        !wbase || wbase > limit - 0x3CF98 - 0x10) return false;
    const bool isFill = paint == Paint::Fill;
    const auto& path = isFill ? fill : stroke;
    const std::array<ULONG_PTR, 5> callers{
        graphics + (isFill ? 0xAC84F : 0xAF1DB), graphics + (isFill ? 0xB46BF : 0xB3121),
        wbase + (isFill ? 0x17466 : 0x174D8), wbase + 0x1E9E0, wbase + 0x1D5D9
    };
    panel_detail::Reader reader;
    ULONG_PTR value{};
    for (size_t i = 0; i < path.returnSlots.size(); ++i)
        if (!reader.stack(c.Rsp, path.returnSlots[i], value) || value != callers[i]) return false;

    BYTE maskMode{};
    ULONG_PTR scroller{}, layout{}, componentSlot{}, componentOffset{}, rect{};
    if (!reader.read(isFill ? c.Rdi : c.Rbx, 0xD9, maskMode) || maskMode ||
        !reader.stack(c.Rsp, path.scroller, scroller) || !scroller || scroller > limit - 0x1C8 ||
        !reader.stack(c.Rsp, path.layout, layout) || layout != scroller + 0xA8 ||
        !reader.stack(c.Rsp, path.componentSlot, componentSlot) || componentSlot != layout + 0x38 ||
        !reader.stack(c.Rsp, path.componentOffset, componentOffset) || componentOffset != 0x70 ||
        !reader.stack(c.Rsp, path.rect, rect) || rect != layout + 0x70)
        return false;

    ULONG_PTR component{}, window{};
    RECT bounds{};
    return reader.read(componentSlot, 0, component) && component == scroller + 0x1C8 &&
        reader.read(component, 0, value) && value == wbase + 0x3CF98 &&
        reader.read(value, 0x10, value) && value == wbase + 0x17400 &&
        reader.read(scroller, 0, value) && value == wbase + 0x3CA78 &&
        reader.read(value, 0xC0, value) && value == wbase + 0x1E860 &&
        reader.read(scroller, 0x20, window) &&
        reader.read(window, 0, value) && value == frame + 0xABEDB0 &&
        reader.read(window, 0xB8, value) && value == scroller &&
        reader.read(rect, 0, bounds) && panel_detail::nonempty(bounds);
}
} // namespace scroller_corner
