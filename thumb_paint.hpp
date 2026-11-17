#pragma once

#include "panel_headers.hpp"
#include "thumb_surface_probe.hpp"
#include <iterator>

// Shared, read-only identity gates for resized scrollbar thumb painting.
// The caller must verify and pin the exact platform images before use.
namespace thumb_paint {
using Stage = ThumbSurfaceStage;
using CacheCounter = ThumbCacheCounter;
inline constexpr ULONG_PTR drawFrame = 0x730, paintFrame = 0x8A0, ownerFrame = 0x9F0;
inline constexpr ULONG_PTR imageDrawFrame = 0x660;
inline constexpr std::array<DWORD, 7> resizedReturns{0x19AF5,0x19B78,0x19BFD,0x19ED0,0x19FAD,0x1A030,0x1A0B5};
struct Images { ULONG_PTR frame{}, graphics{}, wbase{}, imageUi{}; };

inline DWORD drawReturnRva(ULONG_PTR value, ULONG_PTR wbase)
{
    for (const auto draw : resizedReturns) {
        if (value == wbase + draw) return draw;
    }
    return 0;
}

inline Stage delegatedFrames(panel_detail::Reader& r, ULONG_PTR h, ULONG_PTR wbase)
{
    constexpr ULONG_PTR shift = 0xC0;
    ULONG_PTR value{};
    if (!r.stack(h, drawFrame + shift, value)) return Stage::ReadFailure;
    if (!drawReturnRva(value, wbase)) return Stage::DelegatedDraw;
    const ULONG_PTR offsets[]{paintFrame + shift, ownerFrame + shift, ownerFrame + 0x90 + shift};
    const ULONG_PTR returns[]{0x194BE, 0x1E9E0, 0x1D5D9};
    for (size_t i = 0; i < std::size(offsets); ++i) {
        if (!r.stack(h, offsets[i], value)) return Stage::ReadFailure;
        if (value != wbase + returns[i]) return Stage::DelegatedDraw;
    }
    return Stage::DelegatedFrames;
}

struct CacheObservation {
    CacheCounter gate{CacheCounter::Count}, methods{CacheCounter::Count};
};

inline Stage checkCache(panel_detail::Reader& r, ULONG_PTR h, ULONG_PTR component, ULONG_PTR imageUi,
                        CacheObservation& cache)
{
    const auto deny = [&](CacheCounter reason, Stage stage = Stage::Cache) {
        cache.gate = reason;
        return stage;
    };
    ULONG_PTR cached{}, qi{}, drawingObject{}, rasterQi{}, rasterTable{}, qiTable{}, drawMethod{}, rasterMethod{};
    if (!r.read(component, 0x28, cached) || !r.stack(h, drawFrame + 0x80, qi) ||
        !r.stack(h, imageDrawFrame - 0x28, drawingObject) || !r.stack(h, imageDrawFrame - 0x160, rasterQi))
        return deny(CacheCounter::InputsReadFailure, Stage::ReadFailure);
    // The resolved imageui drawing interface is at +8 in its wrapper. The
    // component's image.dll cache and the converted Cairo surface can be separate objects.
    if (!cached) return deny(CacheCounter::NullCache);
    if (qi <= 8) return deny(CacheCounter::InvalidDrawQi);
    if (drawingObject != qi - 8) return deny(CacheCounter::DrawingObjectMismatch);
    if (!rasterQi) return deny(CacheCounter::NullRasterQi);

    // imageui AE04 preserves the raster QI interface, and AE0B dereferences it.
    if (!r.read(qi, 0, qiTable) || !r.read(rasterQi, 0, rasterTable))
        return deny(CacheCounter::TablesReadFailure, Stage::ReadFailure);
    const bool originalDraw = qiTable == imageUi + 0x14218;
    const bool originalRaster = rasterTable == imageUi + 0x13D08;
    if ((!originalDraw && qiTable != imageUi + 0x14090) ||
        (!originalRaster && rasterTable != imageUi + 0x13C80)) return deny(CacheCounter::UnknownPair);
    const bool original = originalDraw && originalRaster;
    cache.gate = original ? CacheCounter::OriginalPair : originalDraw ? CacheCounter::AlternateRasterPair :
        originalRaster ? CacheCounter::AlternateDrawPair : CacheCounter::AlternateBothPair;

    // The original pair and the observed aggregate pair continue to the existing
    // state and geometry checks. Mixed pairs only contribute diagnostics.
    const bool admittedPair = originalDraw == originalRaster;
    if (!r.read(qiTable, 0x18, drawMethod) || !r.read(rasterTable, 0x18, rasterMethod)) {
        cache.methods = CacheCounter::MethodsReadFailure;
        return admittedPair ? Stage::ReadFailure : Stage::Cache;
    }
    const bool drawMatches = drawMethod == imageUi + 0x8FE0;
    const bool rasterMatches = rasterMethod == imageUi + 0x6340;
    cache.methods = drawMatches ? (rasterMatches ? CacheCounter::MethodsMatch : CacheCounter::WrongRasterMethod) :
        (rasterMatches ? CacheCounter::WrongDrawMethod : CacheCounter::WrongBothMethods);
    return admittedPair && drawMatches && rasterMatches ? Stage::Accepted : Stage::Cache;
}

inline Stage locatePaint(ULONG_PTR h, const Images& b, ThumbSurfaceIdentity& id, CacheObservation& cache)
{
    using panel_detail::limit;
    if (!b.frame || b.frame > limit - 0xABEDB0 || !b.graphics || b.graphics > limit - 0xC7BE0 ||
        !b.wbase || b.wbase > limit - 0x3D380 || !b.imageUi || b.imageUi > limit - 0x14280)
        return Stage::ReadFailure;

    panel_detail::Reader r;
    ULONG_PTR value{};
    if (!r.stack(h, 0, value)) return Stage::ReadFailure;
    if (value != b.graphics + 0x13482) return Stage::OtherCall;
    const ULONG_PTR offsets[]{0x260, 0x360, 0x420, 0x490, imageDrawFrame};
    const ULONG_PTR returns[]{b.graphics + 0xAE2A2, b.graphics + 0xB6B09, b.imageUi + 0x63BB,
                              b.imageUi + 0xAE67, b.imageUi + 0x90AB};
    for (size_t i = 0; i < std::size(offsets); ++i) {
        if (!r.stack(h, offsets[i], value)) return Stage::ReadFailure;
        if (value != returns[i]) return Stage::GraphicsChain;
    }
    if (!r.stack(h, drawFrame, value)) return Stage::ReadFailure;
    id.drawReturnRva = drawReturnRva(value, b.wbase);
    if (!id.drawReturnRva) {
        // Count the alternate chain for diagnostics. Sampling requires a
        // separately verified owner and cache layout for that chain.
        return value == b.imageUi + 0x7BD0 ? delegatedFrames(r, h, b.wbase) : Stage::DrawReturn;
    }
    const ULONG_PTR outerOffsets[]{paintFrame, ownerFrame, ownerFrame + 0x90};
    const ULONG_PTR outerReturns[]{0x194BE, 0x1E9E0, 0x1D5D9};
    const Stage outerRejections[]{Stage::PaintReturn, Stage::OwnerReturn, Stage::OuterReturn};
    for (size_t i = 0; i < std::size(outerOffsets); ++i) {
        if (!r.stack(h, outerOffsets[i], value)) return Stage::ReadFailure;
        if (value != b.wbase + outerReturns[i]) return outerRejections[i];
    }

    // F+78 is a reused coordinates temporary. The saved imageui VDC (RSI)
    // belongs to AC50, whose return is at H+660. H+490 is its raster callee's
    // return, so reading H+490-18 would inspect an unrelated flag.
    ULONG_PTR vdc{}, savedVdc{}, imageVdc{};
    if (!r.stack(h, paintFrame + 0x70, vdc) || !r.stack(h, paintFrame - 0x30, savedVdc) ||
        !r.stack(h, imageDrawFrame - 0x18, imageVdc)) return Stage::ReadFailure;
    if (!vdc || vdc != savedVdc || vdc != imageVdc) return Stage::Vdc;

    ULONG_PTR scroller{}, layout{}, offset{}, slot{}, rect{}, config{};
    if (!r.stack(h, ownerFrame - 0x38, scroller) || !r.stack(h, ownerFrame - 0x40, layout) ||
        !r.stack(h, ownerFrame - 0x20, offset) || !r.stack(h, ownerFrame - 0x10, slot) ||
        !r.stack(h, ownerFrame - 0x18, rect) || !r.stack(h, paintFrame - 0x10, config))
        return Stage::ReadFailure;
    if (!scroller || scroller > limit - 0x108 || layout != scroller + 0xA8 || config != scroller + 0x28 ||
        (offset != 0x40 && offset != 0x50) || slot != layout + 0x20 + (offset - 0x40) / 2 ||
        rect != layout + offset) return Stage::Owner;
    ULONG_PTR window{}, table{}, windowTable{}, entry{}, dispatch{}, back{};
    if (!r.read(scroller, 0x20, window) || !r.read(scroller, 0, table)) return Stage::ReadFailure;
    if (!window) return Stage::Owner;
    if (!r.read(window, 0, windowTable) || !r.read(window, 0xB8, back)) return Stage::ReadFailure;
    DWORD expectedDispatch{};
    if (windowTable == b.frame + 0xABEDB0 && table == b.wbase + 0x3CA78) {
        id.windowTableRva = 0xABEDB0;
        id.scrollerTableRva = 0x3CA78;
        expectedDispatch = 0x1E860;
    } else if (windowTable == b.frame + 0xA885D0 && table == b.wbase + 0x3CD00 && offset == 0x40) {
        id.windowTableRva = 0xA885D0;
        id.scrollerTableRva = 0x3CD00;
        expectedDispatch = 0x1EFE0;
    } else return Stage::Owner;
    if (!r.read(table, 0x58, entry) || !r.read(table, 0xC0, dispatch)) return Stage::ReadFailure;
    if (back != scroller || entry != b.wbase + 0x1D600 || dispatch != b.wbase + expectedDispatch)
        return Stage::Owner;

    ULONG_PTR component{}, actual{}, paint{}, getter{}, thumbMetrics{};
    if (!r.stack(h, paintFrame - 0x18, component) || !r.read(slot, 0, actual)) return Stage::ReadFailure;
    if (!component || actual != component) return Stage::Component;
    if (!r.read(component, 0, table)) return Stage::ReadFailure;
    if (table != b.wbase + 0x3D348) return Stage::Component;
    if (!r.read(table, 0x10, paint) || !r.read(table, 0x30, getter) || !r.read(table, 0x38, thumbMetrics) ||
        !r.read(component, 8, id.orientation) || !r.read(component, 0xC, id.hotId) ||
        !r.read(component, 0x10, id.pressedId)) return Stage::ReadFailure;
    if (paint != b.wbase + 0x19290 || getter != b.wbase + 0x1B930 || thumbMetrics != b.wbase + 0x1B960 ||
        id.orientation != (offset - 0x40) / 0x10 || id.hotId > 5 || id.pressedId > 5) return Stage::Component;
    if (id.drawReturnRva != 0x19ED0 && (id.drawReturnRva >= 0x19FAD) != (id.orientation != 0))
        return Stage::Orientation;

    if (const auto stage = checkCache(r, h, component, b.imageUi, cache); stage != Stage::Accepted) return stage;

    BYTE configEnabled{}, interactive{}, parentInteractive{};
    if (!r.read(config, 0x70, configEnabled) || !r.stack(h, paintFrame + 0x28, interactive) ||
        !r.stack(h, ownerFrame + 0x28, parentInteractive) || !r.read(component, 0x30, id.cacheState))
        return Stage::ReadFailure;
    const DWORD expected = !configEnabled ? 1 : !interactive ? 0 : id.pressedId == 5 ? 3 : id.hotId == 5 ? 2 : 0;
    if (interactive != parentInteractive || id.cacheState != expected) return Stage::State;

    RECT bounds{}, thumb{};
    BYTE thumbPresent{};
    if (!r.read(rect, 0, bounds) || !r.stack(h, paintFrame + 0xD8, thumb) ||
        !r.stack(h, paintFrame + 0xEA, thumbPresent)) return Stage::ReadFailure;
    if (thumbPresent != 1) return Stage::ThumbPresent;
    if (!panel_detail::nonempty(bounds) || !panel_detail::nonempty(thumb)) return Stage::Owner;
    id.componentOffset = static_cast<DWORD>(offset);
    id.enabled = configEnabled != 0;
    id.interactive = interactive != 0;
    id.left = bounds.left; id.top = bounds.top; id.right = bounds.right; id.bottom = bounds.bottom;
    id.thumbLeft = thumb.left; id.thumbTop = thumb.top; id.thumbRight = thumb.right; id.thumbBottom = thumb.bottom;
    return Stage::Accepted;
}

// Only surface layouts observed in B2/B3 are admitted for recoloring. The
// general clone also supports transformed/padded images; that does not broaden
// the production owner/part/format gate. Disabled states await live coverage.
inline bool knownSurface(const ThumbSurfaceIdentity& id, int format, int width, int height)
{
    if (id.enabled != 1 || id.interactive != 1 || id.orientation > 1) return false;
    const bool tree = id.windowTableRva == 0xA885D0 && id.scrollerTableRva == 0x3CD00;
    const bool editor = id.windowTableRva == 0xABEDB0 && id.scrollerTableRva == 0x3CA78;
    if ((!tree && !editor) || (tree && id.orientation)) return false;
    int minor{}, major{};
    if (format == 1) {
        minor = 16;
        if (id.drawReturnRva == 0x19ED0) major = id.orientation ? 5 : 6;
        else if ((!id.orientation && id.drawReturnRva == 0x19B78) ||
                 (id.orientation && id.drawReturnRva == 0x1A030)) major = 1;
        else if ((!id.orientation && (id.drawReturnRva == 0x19AF5 || id.drawReturnRva == 0x19BFD)) ||
                 (id.orientation && (id.drawReturnRva == 0x19FAD || id.drawReturnRva == 0x1A0B5))) major = 5;
        else return false;
    } else if (format == 0 && tree) {
        minor = 20;
        if (id.drawReturnRva == 0x19ED0) major = 7;
        else if (id.drawReturnRva == 0x19B78) major = 1;
        else if (id.drawReturnRva == 0x19AF5 || id.drawReturnRva == 0x19BFD) major = 6;
        else return false;
    } else return false;
    return width == (id.orientation ? major : minor) && height == (id.orientation ? minor : major);
}
} // namespace thumb_paint
