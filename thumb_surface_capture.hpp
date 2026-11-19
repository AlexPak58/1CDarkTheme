#pragma once

#include "scroller_png_hook.hpp"
#include "thumb_paint.hpp"
#include <cmath>
#include <iterator>
#include <new>

namespace thumb_surface_capture {
using Stage = ThumbSurfaceStage;
using CacheCounter = ThumbCacheCounter;
inline constexpr ULONGLONG snapshotWaitMs = 100;
using thumb_paint::drawFrame;
using thumb_paint::paintFrame;
using thumb_paint::ownerFrame;
using thumb_paint::imageDrawFrame;
using thumb_paint::resizedReturns;
using thumb_paint::Images;
using thumb_paint::CacheObservation;
using thumb_paint::drawReturnRva;
using thumb_paint::delegatedFrames;
using thumb_paint::checkCache;
using thumb_paint::locatePaint;

// One writer under runLock, published by the deadline's release store. The
// thread running the helper is not involved in opening or closing this window.
inline const scroller_png_hook::Binding* armedBinding{};
inline Images images;
inline SRWLOCK runLock = SRWLOCK_INIT, collectorLock = SRWLOCK_INIT;
inline bool attempted{};
inline std::atomic<ULONGLONG> deadline{}, busyDrops{};
inline ThumbSurfaceTrace observations;
static_assert(decltype(deadline)::is_always_lock_free && decltype(busyDrops)::is_always_lock_free);

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

inline void record(Stage stage, const ThumbSurfaceIdentity& id, const CacheObservation& cache)
{
    ++observations.stages[static_cast<size_t>(stage)];
    for (const auto counter : {cache.gate, cache.methods}) {
        if (counter != CacheCounter::Count) ++observations.cacheCounters[static_cast<size_t>(counter)];
    }
    if (stage != Stage::Accepted) return;
    size_t index{};
    while (index < observations.sampleCount && !sameThumbSurfaceGroup(observations.samples[index].identity, id)) ++index;
    if (index == observations.sampleCount) {
        if (index == observations.samples.size()) { ++observations.capacityDrops; return; }
        observations.samples[index].identity = id;
        observations.samples[index].geometryMin = observations.samples[index].geometryMax = thumbSurfaceGeometry(id);
        ++observations.sampleCount;
    }
    auto& sample = observations.samples[index];
    const auto geometry = thumbSurfaceGeometry(id);
    for (size_t i = 0; i < geometry.size(); ++i) {
        if (geometry[i] < sample.geometryMin[i]) sample.geometryMin[i] = geometry[i];
        if (geometry[i] > sample.geometryMax[i]) sample.geometryMax[i] = geometry[i];
    }
    ++sample.calls;
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
        CacheObservation cache;
        auto stage = locatePaint(h, images, id, cache);
        if (stage == Stage::Accepted && active(end)) stage = readSurface(binding.api, surface, id);
        // An accessor already in progress at expiry cannot publish a sample.
        if (active(end)) record(stage, id, cache);
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
    trace.cacheCounters = observations.cacheCounters;
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
