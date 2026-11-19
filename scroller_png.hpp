#pragma once

#include "panel_headers.hpp"

namespace scroller_png {
struct Identity {
    DWORD part{}, orientation{}, hotId{}, pressedId{};
    bool enabled{true}, interactive{true};
};

// The track and arrow routes accepted by the metadata probe on 8.3.27.2130.
// The caller must admit and pin the exact platform modules before using them.
inline bool identify(ULONG_PTR h, ULONG_PTR frame, ULONG_PTR graphics, ULONG_PTR wbase, Identity& identity)
{
    constexpr ULONG_PTR componentReturn = 0x4C0;
    using panel_detail::limit;
    if (!frame || frame > limit - 0xABF108 || !graphics || graphics > limit - 0x13482 ||
        !wbase || wbase > limit - 0x3D378) return false;
    panel_detail::Reader r;
    ULONG_PTR value{}, return1{}, return2{};
    if (!r.stack(h, 0, value) || value != graphics + 0x13482 ||
        !r.stack(h, 0x260, return1) || return1 != graphics + 0x4911 ||
        !r.stack(h, 0x430, return2) || return2 != graphics + 0x91BA ||
        !r.stack(h, componentReturn, value)) return false;

    Identity matched;
    ULONG_PTR componentSlotOnStack{}, paintReturn{}, arrowReturn{};
    if (value == wbase + 0x19410) {
        matched.part = 3;
        paintReturn = componentReturn + 0x150;
        componentSlotOnStack = componentReturn - 0x08;
    } else if (value == wbase + 0x1963F) {
        arrowReturn = componentReturn + 0x90;
        if (!r.stack(h, arrowReturn, value)) return false;
        if (value == wbase + 0x19452) matched.part = 0;
        else if (value == wbase + 0x1948B) matched.part = 1;
        else return false;
        paintReturn = arrowReturn + 0x150;
        componentSlotOnStack = arrowReturn - 0x10;
    } else {
        return false;
    }

    BYTE drawFlag{};
    if (!r.stack(h, paintReturn, return1) || return1 != wbase + 0x1E9E0 ||
        !r.stack(h, paintReturn + 0x90, return2) || return2 != wbase + 0x1D5D9 ||
        !r.stack(h, paintReturn + 0x28, drawFlag) || drawFlag != 1) return false;

    ULONG_PTR scroller{}, layout{}, offset{}, slot{}, rect{};
    if (!r.stack(h, paintReturn - 0x38, scroller) || !scroller || scroller > limit - 0xF8 ||
        !r.stack(h, paintReturn - 0x40, layout) || layout != scroller + 0xA8 ||
        !r.stack(h, paintReturn - 0x20, offset) || (offset != 0x40 && offset != 0x50) ||
        !r.stack(h, paintReturn - 0x10, slot) || slot != layout + 0x20 + (offset - 0x40) / 2 ||
        !r.stack(h, paintReturn - 0x18, rect) || rect != layout + offset) return false;

    ULONG_PTR window{}, table{}, windowTable{}, entry{}, dispatch{}, back{};
    if (!r.read(scroller, 0x20, window) || !window || !r.read(scroller, 0, table) ||
        !r.read(window, 0, windowTable) || !r.read(window, 0xB8, back) || back != scroller) return false;
    DWORD expectedDispatch{};
    if (windowTable == frame + 0xABF108 && table == wbase + 0x3CA78) {
        expectedDispatch = 0x1E860;
    } else if (windowTable == frame + 0xA885D0 && table == wbase + 0x3CD00 && offset == 0x40) {
        expectedDispatch = 0x1EFE0;
    } else {
        return false;
    }
    if (!r.read(table, 0x58, entry) || entry != wbase + 0x1D600 ||
        !r.read(table, 0xC0, dispatch) || dispatch != wbase + expectedDispatch) return false;

    if (arrowReturn) {
        // wbase+19510 saves the caller's RSI at E-8. Its enabled argument at
        // E+30 is overwritten during painting, so recover the caller's two inputs.
        ULONG_PTR config{};
        BYTE configEnabled{}, disabled{}, interaction{};
        if (!r.stack(h, arrowReturn - 8, config) || config != scroller + 0x28 ||
            !r.read(config, 0x70, configEnabled) ||
            !r.stack(h, arrowReturn + 0xE8 + matched.part, disabled) ||
            !r.stack(h, arrowReturn + 0x38, interaction)) return false;
        matched.enabled = configEnabled != 0 && disabled == 0;
        matched.interactive = interaction != 0;
    }

    ULONG_PTR component{}, actual{}, paint{}, getter{};
    RECT bounds{};
    if (!r.stack(h, componentSlotOnStack, component) || !component ||
        !r.read(slot, 0, actual) || actual != component ||
        !r.read(component, 0, table) || table != wbase + 0x3D348 ||
        !r.read(table, 0x10, paint) || paint != wbase + 0x19290 ||
        !r.read(table, 0x30, getter) || getter != wbase + 0x1B930 ||
        !r.read(component, 8, matched.orientation) || matched.orientation != (offset - 0x40) / 0x10 ||
        !r.read(component, 0xC, matched.hotId) || matched.hotId > 5 ||
        !r.read(component, 0x10, matched.pressedId) || matched.pressedId > 5 ||
        !r.read(rect, 0, bounds) || !panel_detail::nonempty(bounds)) return false;
    identity = matched;
    return true;
}

using GetInt = int (*)(const void*);
using GetPair = void (*)(const void*, double*, double*);
using SurfaceAction = void (*)(void*);
using GetData = unsigned char* (*)(void*);
using ImageCreate = void* (*)(int, int, int);

struct CairoApi {
    GetInt status{}, type{}, format{}, width{}, height{}, stride{};
    GetPair scale{}, offset{};
    SurfaceAction flush{}, destroy{}, markDirty{};
    GetData data{};
    ImageCreate imageCreate{};
};

struct Palette {
    DWORD trackBackground{}, arrowBackground{}, arrowGlyph{}, arrowHover{}, arrowPressed{};
};

namespace detail {
constexpr int imageType = 0;
constexpr int rgb24 = 1;
constexpr size_t maxPixels = 16 * 70;

inline bool exactSurface(const CairoApi& api, const void* surface, int width, int height)
{
    if (api.status(surface) || api.type(surface) != imageType || api.format(surface) != rgb24 ||
        api.width(surface) != width || api.height(surface) != height ||
        api.stride(surface) != width * static_cast<int>(sizeof(DWORD))) return false;
    double scaleX{}, scaleY{}, offsetX{1}, offsetY{1};
    api.scale(surface, &scaleX, &scaleY);
    api.offset(surface, &offsetX, &offsetY);
    return scaleX == 1 && scaleY == 1 && offsetX == 0 && offsetY == 0;
}

inline bool pixelRange(const void* data, size_t bytes, bool writable)
{
    const auto address = reinterpret_cast<ULONG_PTR>(data);
    MEMORY_BASIC_INFORMATION region{};
    if (!address || address % alignof(DWORD) || bytes > panel_detail::limit - address ||
        !VirtualQuery(data, &region, sizeof(region)) || region.State != MEM_COMMIT)
        return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    if (address < start || address - start >= region.RegionSize || bytes > region.RegionSize - (address - start))
        return false;
    return region.Protect == PAGE_READWRITE || region.Protect == PAGE_WRITECOPY ||
        (!writable && region.Protect == PAGE_READONLY);
}

inline DWORD blendArrow(DWORD color, DWORD background, DWORD glyph)
{
    // Cairo RGB24 is native-endian 0x00RRGGBB. Map source luminance into the
    // palette while retaining the relative darkness of outlines and edge pixels.
    const auto luma = (77 * ((color >> 16) & 0xFF) + 150 * ((color >> 8) & 0xFF) + 29 * (color & 0xFF) + 128) / 256;
    const auto weight = luma >= 240 ? 0UL : (luma <= 120 ? 120UL : 240 - luma);
    DWORD result{};
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        const auto bg = (background >> shift) & 0xFF;
        const auto fg = (glyph >> shift) & 0xFF;
        result |= ((bg * (120 - weight) + fg * weight + 60) / 120) << shift;
    }
    return result;
}

inline bool paintPixels(const BYTE* source, BYTE* destination, size_t pixels, DWORD background, DWORD glyph)
{
    const auto bytes = pixels * sizeof(DWORD);
    if (!pixelRange(destination, bytes, true)) return false;
    if (source) {
        if (!pixelRange(source, bytes, false)) return false;
        const auto src = reinterpret_cast<ULONG_PTR>(source), dst = reinterpret_cast<ULONG_PTR>(destination);
        if (src < dst + bytes && dst < src + bytes) return false;
    }
    __try {
        for (size_t i = 0; i < pixels; ++i) {
            const auto offset = i * sizeof(DWORD);
            DWORD color = background & 0x00FFFFFF;
            if (source) {
                std::memcpy(&color, source + offset, sizeof(color));
                color = blendArrow(color, background, glyph);
            }
            std::memcpy(destination + offset, &color, sizeof(color));
        }
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
} // namespace detail

// The input is borrowed. A non-null result is a distinct, owned Cairo image.
inline void* clone(const CairoApi& api, void* source, const Identity& id, const Palette& palette)
{
    if (!source || id.orientation > 1 || id.hotId > 5 || id.pressedId > 5 ||
        (id.part != 0 && id.part != 1 && id.part != 3)) return nullptr;
    const bool track = id.part == 3;
    const int major = track ? 70 : 21;
    const int width = id.orientation ? major : 16;
    const int height = id.orientation ? 16 : major;
    const auto pixels = static_cast<size_t>(width * height);
    if (pixels > detail::maxPixels || !detail::exactSurface(api, source, width, height)) return nullptr;

    const BYTE* sourcePixels{};
    DWORD background = palette.trackBackground;
    if (!track) {
        background = palette.arrowBackground;
        if (id.enabled && id.interactive) {
            const auto arrow = id.part + 1;
            if (id.pressedId == arrow) background = palette.arrowPressed;
            else if (id.hotId == arrow) background = palette.arrowHover;
        }
        api.flush(source);
        if (api.status(source)) return nullptr;
        sourcePixels = api.data(source);
        if (!sourcePixels) return nullptr;
    }

    auto* destination = api.imageCreate(detail::rgb24, width, height);
    if (!destination || destination == source) return nullptr;
    if (detail::exactSurface(api, destination, width, height)) {
        api.flush(destination);
        if (!api.status(destination) &&
            detail::paintPixels(sourcePixels, api.data(destination), pixels, background, palette.arrowGlyph)) {
            api.markDirty(destination);
            if (!api.status(destination)) return destination;
        }
    }
    api.destroy(destination);
    return nullptr;
}
} // namespace scroller_png
