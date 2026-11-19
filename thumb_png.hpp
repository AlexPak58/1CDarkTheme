#pragma once

#include "scroller_png.hpp"
#include <cmath>

namespace thumb_png {
using SetPair = void (*)(void*, double, double);
struct CairoApi {
    scroller_png::CairoApi surface{};
    SetPair setScale{}, setOffset{};
};
struct Palette { DWORD background{}, detail{}; };

namespace detail {
constexpr int argb32 = 0, rgb24 = 1, maxDimension = 256, maxStride = 4096;
struct Image {
    int format{}, width{}, height{}, stride{};
    double scaleX{}, scaleY{}, offsetX{}, offsetY{};
    size_t bytes{};
};
inline bool complete(const CairoApi& api)
{
    const auto& a = api.surface;
    return a.status && a.type && a.format && a.width && a.height && a.stride &&
        a.scale && a.offset && a.flush && a.destroy && a.markDirty && a.data &&
        a.imageCreate && api.setScale && api.setOffset;
}
inline bool describe(const scroller_png::CairoApi& a, void* surface, Image& image)
{
    if (a.status(surface) || a.type(surface) != 0) return false;
    image.format = a.format(surface);
    image.width = a.width(surface); image.height = a.height(surface); image.stride = a.stride(surface);
    if ((image.format != argb32 && image.format != rgb24) || image.width <= 0 || image.height <= 0 ||
        image.width > maxDimension || image.height > maxDimension || image.stride < image.width * 4 ||
        image.stride > maxStride || image.stride % 4) return false;
    image.bytes = static_cast<size_t>(image.stride) * static_cast<size_t>(image.height);
    a.scale(surface, &image.scaleX, &image.scaleY);
    a.offset(surface, &image.offsetX, &image.offsetY);
    return std::isfinite(image.scaleX) && std::isfinite(image.scaleY) &&
        image.scaleX > 0 && image.scaleY > 0 && std::isfinite(image.offsetX) && std::isfinite(image.offsetY);
}
inline bool paint(const BYTE* source, BYTE* destination, const Image& src, const Image& dst, Palette palette)
{
    if (!scroller_png::detail::pixelRange(source, src.bytes, false) ||
        !scroller_png::detail::pixelRange(destination, dst.bytes, true)) return false;
    const auto s = reinterpret_cast<ULONG_PTR>(source), d = reinterpret_cast<ULONG_PTR>(destination);
    if (s < d + dst.bytes && d < s + src.bytes) return false;
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            DWORD color{};
            std::memcpy(&color, source + static_cast<size_t>(y) * src.stride + static_cast<size_t>(x) * 4, 4);
            const DWORD alpha = src.format == argb32 ? color >> 24 : 255;
            DWORD straight{};
            for (unsigned shift = 0; shift <= 16; shift += 8) {
                const auto channel = (color >> shift) & 255;
                if (src.format == argb32 && channel > alpha) return false;
                straight |= (alpha ? (channel * 255 + alpha / 2) / alpha : 0) << shift;
            }
            const auto mappedRgb = scroller_png::detail::blendArrow(straight, palette.background, palette.detail);
            DWORD result = src.format == argb32 ? alpha << 24 : 0;
            for (unsigned shift = 0; shift <= 16; shift += 8)
                result |= ((((mappedRgb >> shift) & 255) * alpha + 127) / 255) << shift;
            std::memcpy(destination + static_cast<size_t>(y) * dst.stride + static_cast<size_t>(x) * 4, &result, 4);
        }
    }
    return true;
}
inline void* attempt(const CairoApi& api, void* source, Palette palette, void*& owned)
{
    const auto& a = api.surface;
    Image src{}, dst{};
    if (!describe(a, source, src)) return nullptr;
    a.flush(source);
    if (a.status(source)) return nullptr;
    const auto* pixels = a.data(source);
    if (!scroller_png::detail::pixelRange(pixels, src.bytes, false)) return nullptr;
    auto* destination = a.imageCreate(src.format, src.width, src.height);
    // A broken factory returning the borrowed source does not transfer ownership.
    if (!destination || destination == source) return nullptr;
    owned = destination;
    if (!describe(a, destination, dst) || dst.format != src.format || dst.width != src.width || dst.height != src.height)
        return nullptr;
    api.setScale(destination, src.scaleX, src.scaleY);
    api.setOffset(destination, src.offsetX, src.offsetY);
    if (!describe(a, destination, dst) || dst.format != src.format || dst.width != src.width || dst.height != src.height ||
        dst.scaleX != src.scaleX || dst.scaleY != src.scaleY ||
        dst.offsetX != src.offsetX || dst.offsetY != src.offsetY) return nullptr;
    a.flush(destination);
    if (a.status(destination) || !paint(pixels, a.data(destination), src, dst, palette)) return nullptr;
    a.markDirty(destination);
    return !a.status(destination) && !a.status(source) ? destination : nullptr;
}
inline int exceptionFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_GUARD_PAGE ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
inline void release(const CairoApi& api, void* owned)
{
    __try { api.surface.destroy(owned); }
    __except (exceptionFilter(GetExceptionCode())) {}
}
} // namespace detail

// Borrowed input; success transfers one distinct owned image reference to caller.
// Source padding is never written; destination padding is left owned by Cairo.
// Route/owner/observed thumb geometry admission belongs to the calling hook.
inline void* clone(const CairoApi& api, void* source, Palette palette)
{
    if (!source || !detail::complete(api)) return nullptr;
    void* owned{};
    void* result{};
    __try { result = detail::attempt(api, source, palette, owned); }
    __except (detail::exceptionFilter(GetExceptionCode())) { result = nullptr; }
    if (!result && owned) detail::release(api, owned);
    return result;
}
} // namespace thumb_png
