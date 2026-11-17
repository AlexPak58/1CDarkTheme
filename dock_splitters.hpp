#pragma once

#include "panel_headers.hpp"

namespace dock_detail {
// Hash-gated 8.3.27.2325 only. At grphcs+AEE4E, recover the enclosing
// frame+92AF30 paint through its saved dispatcher RBP, not its reused RBX.
// No new breakpoint, graphics handle or cache lifetime is introduced.
inline bool matches(const CONTEXT& c, ULONG_PTR frame, ULONG_PTR graphics)
{
    using panel_detail::limit;
    if (!frame || !graphics || frame > limit - 0xB2FC08 || graphics > limit - 0xECA60)
        return false;
    panel_detail::Reader r;
    ULONG_PTR value{}, root{}, draw{}, vdc{}, savedVdc{}, selected{}, owned{}, brush{};
    constexpr std::array<ULONG_PTR, 4> slots{0x98, 0xE8, 0x148, 0x1D8};
    const std::array<ULONG_PTR, 4> callers{
        graphics + 0xAC6DC, graphics + 0xB4202, frame + 0x92AFE9, frame + 0x8E9FE7};
    for (size_t i = 0; i < slots.size(); ++i)
        if (!r.stack(c.Rsp, slots[i], value) || value != callers[i]) return false;
    // B4137 saved the paint's VDC; 92AF30 saved the dispatcher's root and VDC.
    if (!r.stack(c.Rsp, 0x1C8, root) ||
        !r.read(root, 0, value) || value != frame + 0xB2FC08 ||
        !r.read(root, 0xB0, value) || value != frame + 0xB2F0B8 ||
        !r.read(root, 0x188, draw) || draw != frame + 0xB2EFF8 ||
        !r.read(draw, 0x18, value) || value != frame + 0x92AF30 ||
        !r.stack(c.Rsp, 0x160, vdc) || !vdc ||
        !r.stack(c.Rsp, 0x1D0, savedVdc) || savedVdc != vdc ||
        !r.stack(c.Rsp, 0x158, value) || value != graphics + 0xECA60)
        return false;
    RECT full{}, clipped{};
    DWORD width{}, height{}, rop{}, color{};
    BYTE maskMode{};
    if (!r.stack(c.Rsp, 0x190, clipped) || !panel_detail::nonempty(clipped) ||
        !r.read(root, 0x208, full) ||
        clipped.left < full.left || clipped.top < full.top ||
        clipped.right > full.right || clipped.bottom > full.bottom ||
        !r.stack(c.Rsp, 0x170, width) || !r.stack(c.Rsp, 0x178, height) ||
        width > static_cast<DWORD>(LONG_MAX) || height > static_cast<DWORD>(LONG_MAX) ||
        static_cast<LONGLONG>(clipped.right) - clipped.left != width ||
        static_cast<LONGLONG>(clipped.bottom) - clipped.top != height ||
        !r.stack(c.Rsp, 0x180, rop) || rop != 0x00F00021 ||
        !r.read(root, 0x268, color) || color != c.Rdx)
        return false;
    // _VBRUSH is an opaque pool handle. Read the decoded DC/brush only;
    // never dereference the handle or acquire/release a cache entry in VEH.
    return r.read(root, 0x270, owned) && owned &&
        r.read(c.Rdi, 0x90, selected) && selected == owned &&
        r.read(c.Rdi, 0x98, brush) && brush == c.Rbx && brush &&
        r.read(c.Rdi, 0xD9, maskMode) && maskMode == 0;
}
} // namespace dock_detail
