#pragma once

#include "scroller_png_hook.hpp"
#include "thumb_surface_probe.hpp"
#include <cmath>
#include <iterator>
#include <new>

namespace thumb_surface_capture {
using Stage = ThumbSurfaceStage;
inline constexpr ULONGLONG snapshotWaitMs = 100;
inline constexpr ULONG_PTR drawFrame = 0x730, paintFrame = 0x8A0, ownerFrame = 0x9F0;
inline constexpr std::array<DWORD, 7> resizedReturns{0x19AF5,0x19B78,0x19BFD,0x19ED0,0x19FAD,0x1A030,0x1A0B5};

struct Images { ULONG_PTR frame{}, graphics{}, wbase{}, imageUi{}; };

// One writer under runLock, published by the deadline's release store. The
// thread running the helper is not involved in opening or closing this window.
inline const scroller_png_hook::Binding* armedBinding{};
inline Images images;
inline SRWLOCK runLock = SRWLOCK_INIT, collectorLock = SRWLOCK_INIT;
inline bool attempted{};
inline std::atomic<ULONGLONG> deadline{}, busyDrops{};
inline ThumbSurfaceTrace observations;
static_assert(decltype(deadline)::is_always_lock_free && decltype(busyDrops)::is_always_lock_free);

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

inline Stage locatePaint(ULONG_PTR h, const Images& b, ThumbSurfaceIdentity& id)
{
    using panel_detail::limit;
    if (!b.frame || b.frame > limit - 0xABEDB0 || !b.graphics || b.graphics > limit - 0xC7BE0 ||
        !b.wbase || b.wbase > limit - 0x3D380 || !b.imageUi || b.imageUi > limit - 0x14280)
        return Stage::ReadFailure;

    panel_detail::Reader r;
    ULONG_PTR value{};
    if (!r.stack(h, 0, value)) return Stage::ReadFailure;
    if (value != b.graphics + 0x13482) return Stage::OtherCall;
    const ULONG_PTR offsets[]{0x260, 0x360, 0x420, 0x490, 0x660};
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

    // F+78 is a reused coordinates temporary. These three VDC copies are
    // preserved along the matched imageui and wbase frames.
    ULONG_PTR vdc{}, savedVdc{}, imageVdc{};
    if (!r.stack(h, paintFrame + 0x70, vdc) || !r.stack(h, paintFrame - 0x30, savedVdc) ||
        !r.stack(h, 0x478, imageVdc)) return Stage::ReadFailure;
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

    ULONG_PTR cached{}, qi{}, drawingObject{}, rasterQi{}, rasterTable{}, qiTable{}, drawMethod{}, rasterMethod{};
    if (!r.read(component, 0x28, cached) || !r.stack(h, drawFrame + 0x80, qi) ||
        !r.stack(h, 0x468, drawingObject) || !r.stack(h, 0x330, rasterQi)) return Stage::ReadFailure;
    // The resolved imageui drawing interface is at +8 in its wrapper. The
    // component's image.dll cache and the converted Cairo surface can be separate objects.
    if (!cached || qi <= 8 || drawingObject != qi - 8 || !rasterQi) return Stage::Cache;
    // imageui AE04 preserves the raster QI interface, and AE0B dereferences it.
    if (!r.read(qi, 0, qiTable) || !r.read(rasterQi, 0, rasterTable)) return Stage::ReadFailure;
    if (qiTable != b.imageUi + 0x14218 || rasterTable != b.imageUi + 0x13D08) return Stage::Cache;
    if (!r.read(qiTable, 0x18, drawMethod) || !r.read(rasterTable, 0x18, rasterMethod)) return Stage::ReadFailure;
    if (drawMethod != b.imageUi + 0x8FE0 || rasterMethod != b.imageUi + 0x6340) return Stage::Cache;

    BYTE enabled{}, interactive{}, parentInteractive{};
    if (!r.read(config, 0x70, enabled) || !r.stack(h, paintFrame + 0x28, interactive) ||
        !r.stack(h, ownerFrame + 0x28, parentInteractive) || !r.read(component, 0x30, id.cacheState))
        return Stage::ReadFailure;
    const DWORD expected = !enabled ? 1 : !interactive ? 0 : id.pressedId == 5 ? 3 : id.hotId == 5 ? 2 : 0;
    if (interactive != parentInteractive || id.cacheState != expected) return Stage::State;

    RECT bounds{}, thumb{};
    BYTE thumbPresent{};
    if (!r.read(rect, 0, bounds) || !r.stack(h, paintFrame + 0xD8, thumb) ||
        !r.stack(h, paintFrame + 0xEA, thumbPresent)) return Stage::ReadFailure;
    if (thumbPresent != 1) return Stage::ThumbPresent;
    if (!panel_detail::nonempty(bounds) || !panel_detail::nonempty(thumb)) return Stage::Owner;
    id.componentOffset = static_cast<DWORD>(offset);
    id.enabled = enabled != 0;
    id.interactive = interactive != 0;
    id.left = bounds.left; id.top = bounds.top; id.right = bounds.right; id.bottom = bounds.bottom;
    id.thumbLeft = thumb.left; id.thumbTop = thumb.top; id.thumbRight = thumb.right; id.thumbBottom = thumb.bottom;
    return Stage::Accepted;
}

inline int getterExceptionFilter(DWORD error)
{
    return error == EXCEPTION_ACCESS_VIOLATION || error == EXCEPTION_IN_PAGE_ERROR || error == EXCEPTION_GUARD_PAGE ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

inline Stage readStatusAndType(const scroller_png::CairoApi& api, const void* surface, ThumbSurfaceIdentity& id)
{
    __try {
        id.status = api.status(surface);
        if (id.status) return Stage::SurfaceError;
        id.surfaceType = api.type(surface);
        if (id.surfaceType) return Stage::SurfaceType;
    } __except (getterExceptionFilter(GetExceptionCode())) { return Stage::ReadFailure; }
    return Stage::Accepted;
}

inline Stage readImageGetters(const scroller_png::CairoApi& api, const void* surface, ThumbSurfaceIdentity& id)
{
    __try {
        id.format = api.format(surface);
        id.width = api.width(surface);
        id.height = api.height(surface);
        id.stride = api.stride(surface);
        api.scale(surface, &id.deviceScaleX, &id.deviceScaleY);
        api.offset(surface, &id.deviceOffsetX, &id.deviceOffsetY);
    } __except (getterExceptionFilter(GetExceptionCode())) { return Stage::ReadFailure; }
    return Stage::Accepted;
}

inline Stage readSurface(const scroller_png::CairoApi& api, const void* surface, ThumbSurfaceIdentity& id)
{
    if (!surface) return Stage::SurfaceError;
    panel_detail::Reader r;
    const auto address = reinterpret_cast<ULONG_PTR>(surface);
    // These are the exact metadata reads in the pinned Cairo PE. Refuse known
    // guard/unreadable ranges before invoking the getters; SEH covers races.
    DWORD word{};
    if (!r.read(address, 0x20, word) || !r.read(address, 0x10, word)) return Stage::ReadFailure;
    auto stage = readStatusAndType(api, surface, id);
    if (stage != Stage::Accepted) return stage;
    ULONG_PTR backend{};
    if (!r.read(address, 0, backend) || !r.read(backend, 0, word)) return Stage::ReadFailure;
    if (word != 0) return Stage::SurfaceType;
    for (const auto slot : {0x16C, 0x178, 0x17C, 0x180}) {
        if (!r.read(address, static_cast<ULONG_PTR>(slot), word)) return Stage::ReadFailure;
    }
    ULONGLONG pairMember{};
    for (const auto slot : {0x70, 0x88, 0x90, 0x98}) {
        if (!r.read(address, static_cast<ULONG_PTR>(slot), pairMember)) return Stage::ReadFailure;
    }
    stage = readImageGetters(api, surface, id);
    if (stage != Stage::Accepted) return stage;
    constexpr std::array<LONG, 6> bitsPerPixel{32, 32, 8, 1, 16, 32};
    if (id.format < 0 || static_cast<size_t>(id.format) >= bitsPerPixel.size() ||
        id.width <= 0 || id.width > 32767 || id.height <= 0 || id.height > 32767 ||
        id.stride <= 0 || id.stride > (1L << 20) || id.stride % 4 ||
        id.stride < (id.width * bitsPerPixel[static_cast<size_t>(id.format)] + 7) / 8 ||
        !std::isfinite(id.deviceScaleX) || !std::isfinite(id.deviceScaleY) ||
        id.deviceScaleX <= 0 || id.deviceScaleY <= 0 ||
        !std::isfinite(id.deviceOffsetX) || !std::isfinite(id.deviceOffsetY)) return Stage::InvalidMetrics;
    return Stage::Accepted;
}

inline void record(Stage stage, const ThumbSurfaceIdentity& id)
{
    ++observations.stages[static_cast<size_t>(stage)];
    if (stage != Stage::Accepted) return;
    size_t index{};
    while (index < observations.sampleCount && !(observations.samples[index].identity == id)) ++index;
    if (index == observations.sampleCount) {
        if (index == observations.samples.size()) { ++observations.capacityDrops; return; }
        observations.samples[index].identity = id;
        ++observations.sampleCount;
    }
    ++observations.samples[index].calls;
}

inline bool active(ULONGLONG end)
{
    return end && GetTickCount64() < end && deadline.load(std::memory_order_acquire) == end;
}

inline void observe(ULONG_PTR h, void* surface, const scroller_png_hook::Binding& binding)
{
    const auto end = deadline.load(std::memory_order_acquire);
    if (!active(end) || armedBinding != &binding) return;
    if (!TryAcquireSRWLockExclusive(&collectorLock)) {
        if (active(end)) busyDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (active(end)) {
        ThumbSurfaceIdentity id{};
        auto stage = locatePaint(h, images, id);
        if (stage == Stage::Accepted && active(end)) stage = readSurface(binding.api, surface, id);
        // An accessor already in progress at expiry cannot publish a sample.
        if (active(end)) record(stage, id);
    }
    ReleaseSRWLockExclusive(&collectorLock);
}

inline DWORD copySamples(ThumbSurfaceTrace& trace)
{
    const auto start = GetTickCount64();
    while (!TryAcquireSRWLockExclusive(&collectorLock)) {
        if (GetTickCount64() - start >= snapshotWaitMs) return ERROR_TIMEOUT;
        Sleep(1);
    }
    trace.sampleCount = observations.sampleCount;
    trace.stages = observations.stages;
    trace.samples = observations.samples;
    trace.busyDrops = busyDrops.load(std::memory_order_relaxed);
    trace.capacityDrops = observations.capacityDrops;
    ReleaseSRWLockExclusive(&collectorLock);
    return ERROR_SUCCESS;
}

inline DWORD bindingStatus(const scroller_png_hook::Binding& binding)
{
    if (scroller_png_hook::published.load(std::memory_order_acquire) != &binding) return ERROR_INVALID_STATE;
    if (!scroller_png_hook::slotEquals(binding, reinterpret_cast<PVOID>(&scroller_png_hook::createPattern)))
        return ERROR_INVALID_DATA;
    const auto* profile = binding.profile ? binding.profile->load(std::memory_order_acquire) : nullptr;
    if (!profile || !(profile->zones & enableScrollerPng)) return ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

struct ModuleRef {
    HMODULE value{};
    ~ModuleRef() { if (value) FreeLibrary(value); }
    bool open(LPCWSTR name) { return GetModuleHandleExW(0, name, &value) != FALSE; }
    ULONG_PTR base() const { return reinterpret_cast<ULONG_PTR>(value); }
};

inline DWORD prepare(const scroller_png_hook::Binding& binding, Images& prepared)
{
    if (auto error = bindingStatus(binding)) return error;
    ModuleRef frame, graphics, wbase, cairo, imageUi, image;
    if (!frame.open(L"frame.dll") || !graphics.open(L"grphcs.dll") || !wbase.open(L"wbase83.dll") ||
        !cairo.open(L"cairo.dll") || !imageUi.open(L"imageui.dll") || !image.open(L"image.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (frame.base() != binding.frame || graphics.base() != binding.graphics || wbase.base() != binding.wbase ||
        cairo.base() != binding.cairo || !matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.value), frameHash) || !matchesSha256(modulePath(graphics.value), graphicsHash) ||
        !matchesSha256(modulePath(wbase.value), wbaseHash) || !matchesSha256(modulePath(cairo.value), cairoHash) ||
        !matchesSha256(modulePath(imageUi.value), thumbSurfaceImageUiHash) ||
        !matchesSha256(modulePath(image.value), thumbSurfaceImageHash)) return ERROR_REVISION_MISMATCH;
    constexpr DWORD pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    for (const auto address : {imageUi.base(), image.base()}) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(address), &pinned))
            return scroller_png_hook::errorOr(ERROR_DLL_INIT_FAILED);
    }
    prepared = {frame.base(), graphics.base(), wbase.base(), imageUi.base()};
    return ERROR_SUCCESS;
}

inline bool validInput(const void* output)
{
    const auto address = reinterpret_cast<ULONG_PTR>(output);
    MEMORY_BASIC_INFORMATION region{};
    if (!address || address % alignof(ThumbSurfaceTrace) || !VirtualQuery(output, &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY)) return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    if (address < start || address - start >= region.RegionSize ||
        sizeof(ThumbSurfaceTrace) > region.RegionSize - (address - start)) return false;
    DWORD header[2]{};
    return panel_detail::copy(header, address, sizeof(header)) &&
        header[0] == sizeof(ThumbSurfaceTrace) && header[1] == thumbSurfaceProbeVersion;
}

inline DWORD resultOf(const ThumbSurfaceTrace& trace)
{
    return trace.endError ? trace.endError : trace.startError ? trace.startError : trace.snapshotError;
}

inline DWORD runCapture(ThumbSurfaceTrace& trace)
{
    if (attempted) return trace.startError = ERROR_ALREADY_EXISTS;
    attempted = true;
    if (IsDebuggerPresent()) return trace.startError = ERROR_BUSY;
    const auto* binding = scroller_png_hook::published.load(std::memory_order_acquire);
    if (!binding) return trace.startError = ERROR_NOT_READY;
    Images prepared;
    try { trace.startError = prepare(*binding, prepared); }
    catch (const std::bad_alloc&) { trace.startError = ERROR_NOT_ENOUGH_MEMORY; }
    if (trace.startError) return trace.startError;
    trace.startError = IsDebuggerPresent() ? ERROR_BUSY : bindingStatus(*binding);
    if (trace.startError) return trace.startError;

    armedBinding = binding;
    images = prepared;
    const auto start = GetTickCount64(), end = start + thumbSurfaceCaptureMs;
    deadline.store(end, std::memory_order_release);
    ULONGLONG now{};
    while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
    // Close before any potentially failing postcondition, snapshot or output.
    deadline.store(0, std::memory_order_release);
    const auto elapsed = now - start;
    trace.elapsedMs = elapsed > MAXDWORD ? MAXDWORD : static_cast<DWORD>(elapsed);
    trace.endError = bindingStatus(*binding);
    trace.snapshotError = copySamples(trace);
    return resultOf(trace);
}

inline DWORD run(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    ThumbSurfaceTrace trace;
    if (TryAcquireSRWLockExclusive(&runLock)) {
        runCapture(trace);
        ReleaseSRWLockExclusive(&runLock);
    } else trace.startError = ERROR_BUSY;
    const auto result = resultOf(trace);
    if (!panel_detail::copy(output, reinterpret_cast<ULONG_PTR>(&trace), sizeof(trace)))
        return result ? result : ERROR_NOACCESS;
    return result;
}
} // namespace thumb_surface_capture
