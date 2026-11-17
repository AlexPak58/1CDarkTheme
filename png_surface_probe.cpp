#include "png_surface_probe.hpp"
#include "panel_headers.hpp"

#include <atomic>
#include <cmath>
#include <intrin.h>
#include <new>

namespace {
using Stage = PngSurfaceStage;
using PatternCreate = void* (*)(void*);
using GetInt = int (*)(const void*);
using GetPair = void (*)(const void*, double*, double*);
constexpr ULONGLONG snapshotWaitMs = 100;

struct CairoApi {
    PatternCreate create{};
    GetInt status{}, type{}, format{}, width{}, height{}, stride{};
    GetPair scale{}, offset{};
};

struct ProbeBindings {
    ULONG_PTR frame{}, graphics{}, wbase{};
    CairoApi cairo;
};

SRWLOCK runLock = SRWLOCK_INIT;
SRWLOCK collectorLock = SRWLOCK_INIT;
bool attempted{};
ProbeBindings bindings;
std::atomic<const ProbeBindings*> published{};
std::atomic<ULONGLONG> deadline{};
std::atomic<ULONGLONG> busyDrops{};
PngSurfaceTrace observations;

static_assert(decltype(published)::is_always_lock_free && decltype(deadline)::is_always_lock_free);

Stage locatePaint(ULONG_PTR h, const ProbeBindings& binding, PngSurfaceIdentity& id)
{
    constexpr ULONG_PTR componentReturn = 0x4C0;
    const auto frame = binding.frame, graphics = binding.graphics, wbase = binding.wbase;
    using panel_detail::limit;
    if (!frame || frame > limit - 0xABEDB0 || !graphics || graphics > limit - 0x13482 ||
        !wbase || wbase > limit - 0x3D378) return Stage::ReadFailure;
    panel_detail::Reader r;
    ULONG_PTR value{}, return1{}, return2{};
    if (!r.stack(h, 0, value)) return Stage::ReadFailure;
    if (value != graphics + 0x13482) return Stage::OtherCall;
    if (!r.stack(h, 0x260, return1) || !r.stack(h, 0x430, return2)) return Stage::ReadFailure;
    if (return1 != graphics + 0x4911 || return2 != graphics + 0x91BA) return Stage::GraphicsChain;
    if (!r.stack(h, componentReturn, value)) return Stage::ReadFailure;

    ULONG_PTR componentSlotOnStack{}, paintReturn{};
    if (value == wbase + 0x19410) {
        id.part = 3;
        paintReturn = componentReturn + 0x150;
        componentSlotOnStack = componentReturn - 0x08;
    } else if (value == wbase + 0x1963F) {
        const auto arrowReturn = componentReturn + 0x90;
        if (!r.stack(h, arrowReturn, value)) return Stage::ReadFailure;
        if (value == wbase + 0x19452) id.part = 0;
        else if (value == wbase + 0x1948B) id.part = 1;
        else return Stage::PaintRoute;
        paintReturn = arrowReturn + 0x150;
        componentSlotOnStack = arrowReturn - 0x10;
    } else {
        // The thumb's offscreen staging draw is outside this observation.
        return Stage::PaintRoute;
    }

    BYTE drawFlag{};
    if (!r.stack(h, paintReturn, return1) || !r.stack(h, paintReturn + 0x90, return2) ||
        !r.stack(h, paintReturn + 0x28, drawFlag)) return Stage::ReadFailure;
    if (return1 != wbase + 0x1E9E0 || return2 != wbase + 0x1D5D9 || drawFlag != 1)
        return Stage::PaintRoute;

    ULONG_PTR scroller{}, layout{}, offset{}, slot{}, rect{};
    if (!r.stack(h, paintReturn - 0x38, scroller) || !r.stack(h, paintReturn - 0x40, layout) ||
        !r.stack(h, paintReturn - 0x20, offset) || !r.stack(h, paintReturn - 0x10, slot) ||
        !r.stack(h, paintReturn - 0x18, rect)) return Stage::ReadFailure;
    if (!scroller || scroller > limit - 0xF8 || layout != scroller + 0xA8 ||
        (offset != 0x40 && offset != 0x50) || slot != layout + 0x20 + (offset - 0x40) / 2 ||
        rect != layout + offset) return Stage::Owner;

    ULONG_PTR window{}, table{}, windowTable{}, entry{}, dispatch{}, back{};
    if (!r.read(scroller, 0x20, window) || !r.read(scroller, 0, table)) return Stage::ReadFailure;
    if (!window) return Stage::Owner;
    if (!r.read(window, 0, windowTable) || !r.read(window, 0xB8, back)) return Stage::ReadFailure;
    DWORD expectedDispatch{};
    if (windowTable == frame + 0xABEDB0 && table == wbase + 0x3CA78) {
        id.windowTableRva = 0xABEDB0;
        id.scrollerTableRva = 0x3CA78;
        expectedDispatch = 0x1E860;
    } else if (windowTable == frame + 0xA885D0 && table == wbase + 0x3CD00 && offset == 0x40) {
        id.windowTableRva = 0xA885D0;
        id.scrollerTableRva = 0x3CD00;
        expectedDispatch = 0x1EFE0;
    } else {
        return Stage::Owner;
    }
    if (!r.read(table, 0x58, entry) || !r.read(table, 0xC0, dispatch)) return Stage::ReadFailure;
    if (back != scroller || entry != wbase + 0x1D600 || dispatch != wbase + expectedDispatch)
        return Stage::Owner;

    ULONG_PTR component{}, actual{}, paint{}, getter{};
    if (!r.stack(h, componentSlotOnStack, component) || !r.read(slot, 0, actual)) return Stage::ReadFailure;
    if (!component || actual != component) return Stage::Component;
    if (!r.read(component, 0, table)) return Stage::ReadFailure;
    if (table != wbase + 0x3D348) return Stage::Component;
    if (!r.read(table, 0x10, paint) || !r.read(table, 0x30, getter) ||
        !r.read(component, 8, id.orientation) || !r.read(component, 0xC, id.hotId) ||
        !r.read(component, 0x10, id.pressedId)) return Stage::ReadFailure;
    if (paint != wbase + 0x19290 || getter != wbase + 0x1B930 ||
        id.orientation != (offset - 0x40) / 0x10 || id.hotId > 5 || id.pressedId > 5)
        return Stage::Component;
    RECT bounds{};
    if (!r.read(rect, 0, bounds)) return Stage::ReadFailure;
    if (!panel_detail::nonempty(bounds)) return Stage::Owner;
    id.componentOffset = static_cast<DWORD>(offset);
    id.left = bounds.left;
    id.top = bounds.top;
    id.right = bounds.right;
    id.bottom = bounds.bottom;
    return Stage::Accepted;
}

Stage readSurface(const CairoApi& api, const void* surface, PngSurfaceIdentity& id)
{
    if (!surface) return Stage::SurfaceError;
    id.status = api.status(surface);
    if (id.status != 0) return Stage::SurfaceError;
    id.surfaceType = api.type(surface);
    if (id.surfaceType != 0) return Stage::SurfaceType;
    id.format = api.format(surface);
    id.width = api.width(surface);
    id.height = api.height(surface);
    id.stride = api.stride(surface);
    api.scale(surface, &id.deviceScaleX, &id.deviceScaleY);
    api.offset(surface, &id.deviceOffsetX, &id.deviceOffsetY);
    constexpr std::array<LONG, 6> bitsPerPixel{32, 32, 8, 1, 16, 32};
    if (id.format < 0 || static_cast<size_t>(id.format) >= bitsPerPixel.size() ||
        id.width <= 0 || id.width > 32767 || id.height <= 0 || id.height > 32767 ||
        id.stride <= 0 || id.stride > (1L << 20) || id.stride % 4 ||
        id.stride < (id.width * bitsPerPixel[static_cast<size_t>(id.format)] + 7) / 8 ||
        !std::isfinite(id.deviceScaleX) || !std::isfinite(id.deviceScaleY) ||
        id.deviceScaleX <= 0 || id.deviceScaleY <= 0 ||
        !std::isfinite(id.deviceOffsetX) || !std::isfinite(id.deviceOffsetY))
        return Stage::InvalidMetrics;
    return Stage::Accepted;
}

void record(ULONG_PTR h, const void* surface, const ProbeBindings& binding)
{
    PngSurfaceIdentity id{};
    auto stage = locatePaint(h, binding, id);
    if (stage == Stage::Accepted) stage = readSurface(binding.cairo, surface, id);
    ++observations.stages[static_cast<size_t>(stage)];
    if (stage == Stage::Accepted) {
        size_t index{};
        while (index < observations.sampleCount && !(observations.samples[index].identity == id)) ++index;
        if (index == observations.sampleCount && index == observations.samples.size()) {
            ++observations.capacityDrops;
            return;
        }
        if (index == observations.sampleCount) observations.samples[observations.sampleCount++] = {id, 1};
        else ++observations.samples[index].calls;
    }
}

void* createPattern(void* surface)
{
    const auto& binding = *published.load(std::memory_order_acquire);
    const auto until = deadline.load(std::memory_order_relaxed);
    if (until) {
        const auto incomingError = GetLastError();
        if (GetTickCount64() < until) {
            if (TryAcquireSRWLockExclusive(&collectorLock)) {
                const auto active = deadline.load(std::memory_order_relaxed);
                if (active && GetTickCount64() < active)
                    record(reinterpret_cast<ULONG_PTR>(_AddressOfReturnAddress()), surface, binding);
                ReleaseSRWLockExclusive(&collectorLock);
            } else {
                busyDrops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        SetLastError(incomingError);
    }
    return binding.cairo.create(surface);
}

struct IatPatch {
    ULONG_PTR graphics{};
    PVOID volatile* slot{};
    PVOID original{};
    bool installed{};
};

bool contains(const MEMORY_BASIC_INFORMATION& region, ULONG_PTR address, SIZE_T length)
{
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return address >= start && address - start < region.RegionSize &&
        length <= region.RegionSize - (address - start);
}

bool inspectIat(const IatPatch& patch, DWORD& protection)
{
    MEMORY_BASIC_INFORMATION region{};
    const auto address = reinterpret_cast<ULONG_PTR>(patch.slot);
    const auto* target = reinterpret_cast<LPCVOID>(address);
    if (!address || address % alignof(PVOID) || !VirtualQuery(target, &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) != patch.graphics ||
        !contains(region, address, sizeof(PVOID))) return false;
    protection = region.Protect;
    return protection == PAGE_READONLY;
}

bool verifyIat(const IatPatch& patch, PVOID expected, DWORD& protection)
{
    PVOID current{};
    return inspectIat(patch, protection) &&
        panel_detail::copy(&current, reinterpret_cast<ULONG_PTR>(patch.slot), sizeof(current)) &&
        current == expected;
}

bool swapSlot(PVOID volatile* slot, PVOID desired, PVOID expected, PVOID& previous)
{
    __try {
        previous = InterlockedCompareExchangePointer(slot, desired, expected);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

DWORD lastErrorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

DWORD changeSlot(IatPatch& patch, bool installing)
{
    DWORD protection{};
    if (!inspectIat(patch, protection)) return ERROR_INVALID_STATE;
    auto* address = reinterpret_cast<LPVOID>(reinterpret_cast<ULONG_PTR>(patch.slot));
    if (!VirtualProtect(address, sizeof(PVOID), PAGE_READWRITE, &protection))
        return lastErrorOr(ERROR_ACCESS_DENIED);

    const auto wrapper = reinterpret_cast<PVOID>(&createPattern);
    const auto expected = installing ? patch.original : wrapper;
    const auto desired = installing ? wrapper : patch.original;
    DWORD error{};
    PVOID previous{};
    if (protection != PAGE_READONLY) error = ERROR_INVALID_DATA;
    else if (!swapSlot(patch.slot, desired, expected, previous)) error = ERROR_NOACCESS;
    else if (previous != expected) {
        error = ERROR_INVALID_DATA;
        if (!installing) patch.installed = false;
    } else {
        patch.installed = installing;
    }

    DWORD ignored{};
    if (!VirtualProtect(address, sizeof(PVOID), protection, &ignored)) {
        const auto restoreError = lastErrorOr(ERROR_ACCESS_DENIED);
        // Roll back only our installed pointer. An unhook already completed
        // must never be reversed just because restoring protection failed.
        if (installing && patch.installed &&
            swapSlot(patch.slot, patch.original, wrapper, previous)) patch.installed = false;
        VirtualProtect(address, sizeof(PVOID), protection, &ignored);
        return error ? error : restoreError;
    }
    return error;
}

struct ModuleRef {
    HMODULE value{};
    ~ModuleRef() { if (value) FreeLibrary(value); }
    bool open(LPCWSTR name) { return GetModuleHandleExW(0, name, &value) != FALSE; }
    ULONG_PTR base() const { return reinterpret_cast<ULONG_PTR>(value); }
};

template<class Function>
bool resolve(const ModuleRef& module, const char* name, Function& function)
{
    const auto address = GetProcAddress(module.value, name);
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.AllocationBase != module.value ||
        (region.Protect != PAGE_EXECUTE_READ && region.Protect != PAGE_EXECUTE)) return false;
    function = reinterpret_cast<Function>(address);
    return true;
}

DWORD prepare(IatPatch& patch, PngSurfaceTrace& trace)
{
    ModuleRef frame, graphics, wbase, cairo;
    if (!frame.open(L"frame.dll") || !graphics.open(L"grphcs.dll") ||
        !wbase.open(L"wbase83.dll") || !cairo.open(L"cairo.dll")) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) || !matchesSha256(modulePath(frame.value), frameHash) ||
        !matchesSha256(modulePath(graphics.value), graphicsHash) ||
        !matchesSha256(modulePath(wbase.value), wbaseHash) ||
        !matchesSha256(modulePath(cairo.value), pngSurfaceCairoHash)) return ERROR_REVISION_MISMATCH;
    if (graphics.base() > panel_detail::limit - pngSurfaceIatRva ||
        cairo.base() > panel_detail::limit - pngSurfaceOriginalRva) return ERROR_INVALID_ADDRESS;
    ProbeBindings candidate{frame.base(), graphics.base(), wbase.base(), {}};
    auto& api = candidate.cairo;
    if (!resolve(cairo, "cairo_pattern_create_for_surface", api.create) ||
        reinterpret_cast<ULONG_PTR>(api.create) != cairo.base() + pngSurfaceOriginalRva ||
        !resolve(cairo, "cairo_surface_status", api.status) ||
        !resolve(cairo, "cairo_surface_get_type", api.type) ||
        !resolve(cairo, "cairo_image_surface_get_format", api.format) ||
        !resolve(cairo, "cairo_image_surface_get_width", api.width) ||
        !resolve(cairo, "cairo_image_surface_get_height", api.height) ||
        !resolve(cairo, "cairo_image_surface_get_stride", api.stride) ||
        !resolve(cairo, "cairo_surface_get_device_scale", api.scale) ||
        !resolve(cairo, "cairo_surface_get_device_offset", api.offset)) return ERROR_PROC_NOT_FOUND;

    patch = {graphics.base(), reinterpret_cast<PVOID volatile*>(graphics.base() + pngSurfaceIatRva),
             reinterpret_cast<PVOID>(api.create)};
    if (!verifyIat(patch, patch.original, trace.originalProtection)) return ERROR_INVALID_DATA;
    constexpr DWORD pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    const ULONG_PTR addresses[]{reinterpret_cast<ULONG_PTR>(&createPattern), frame.base(), graphics.base(),
                                wbase.base(), cairo.base()};
    for (const auto address : addresses) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(address), &pinned))
            return lastErrorOr(ERROR_DLL_INIT_FAILED);
    }
    bindings = candidate;
    published.store(&bindings, std::memory_order_release);
    return ERROR_SUCCESS;
}

void finishPatch(IatPatch& patch, PngSurfaceTrace& trace)
{
    // Teardown must not wait for a collector that is still in a Cairo accessor.
    deadline.store(0, std::memory_order_release);
    DWORD latestError{};
    if (patch.installed) {
        const auto firstError = changeSlot(patch, false);
        latestError = firstError;
        if (firstError) {
            // A recovered transition still faults this observation. Report
            // cleanup ambiguity separately, based on the final IAT state.
            trace.snapshotError = firstError;
            DWORD protection{};
            const auto wrapper = reinterpret_cast<PVOID>(&createPattern);
            if (patch.installed && verifyIat(patch, wrapper, protection))
                latestError = changeSlot(patch, false);
        }
    }
    if (!verifyIat(patch, patch.original, trace.finalProtection))
        trace.cleanupError = latestError ? latestError : ERROR_INVALID_DATA;
}

DWORD copySamples(PngSurfaceTrace& trace)
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

DWORD runProbe(PngSurfaceTrace& trace)
{
    if (attempted) return trace.patchError = ERROR_ALREADY_EXISTS;
    attempted = true;
    if (IsDebuggerPresent()) return trace.patchError = ERROR_BUSY;
    IatPatch patch;
    try {
        trace.patchError = prepare(patch, trace);
    } catch (const std::bad_alloc&) {
        trace.patchError = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (trace.patchError) return trace.patchError;
    trace.patchError = IsDebuggerPresent() ? ERROR_BUSY : changeSlot(patch, true);
    DWORD protection{};
    if (!trace.patchError && !verifyIat(patch, reinterpret_cast<PVOID>(&createPattern), protection))
        trace.patchError = ERROR_INVALID_DATA;
    if (!trace.patchError) {
        const auto start = GetTickCount64();
        const auto end = start + pngSurfaceCaptureMs;
        deadline.store(end, std::memory_order_release);
        ULONGLONG now{};
        while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
        const auto elapsed = now - start;
        trace.elapsedMs = elapsed > MAXDWORD ? MAXDWORD : static_cast<DWORD>(elapsed);
    }
    finishPatch(patch, trace);
    if (trace.cleanupError) return trace.cleanupError;
    if (trace.patchError) return trace.patchError;
    if (!trace.snapshotError) trace.snapshotError = copySamples(trace);
    return trace.snapshotError;
}

bool validInput(const void* output)
{
    const auto address = reinterpret_cast<ULONG_PTR>(output);
    MEMORY_BASIC_INFORMATION region{};
    if (!address || address % alignof(PngSurfaceTrace) || !VirtualQuery(output, &region, sizeof(region)) ||
        region.State != MEM_COMMIT || !contains(region, address, sizeof(PngSurfaceTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY)) return false;
    DWORD header[2]{};
    return panel_detail::copy(header, address, sizeof(header)) &&
        header[0] == sizeof(PngSurfaceTrace) && header[1] == pngSurfaceProbeVersion;
}

DWORD publish(void* output, const PngSurfaceTrace& trace, DWORD result)
{
    return panel_detail::copy(output, reinterpret_cast<ULONG_PTR>(&trace), sizeof(trace)) ?
        result : (result ? result : ERROR_NOACCESS);
}
} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI ScrollerPngSurfaceProbeAbiV1(void*)
{
    return pngSurfaceProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunScrollerPngSurfaceProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    PngSurfaceTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
