#pragma once

#include "scroller_png.hpp"
#include "thumb_png.hpp"
#include "thumb_paint.hpp"
#include "attach_process.hpp"
#include <atomic>
#include <intrin.h>

#ifdef THEME_THUMB_SURFACE_PROBE
namespace scroller_png_hook { struct Binding; }
namespace thumb_surface_capture {
inline void observe(ULONG_PTR, void*, const scroller_png_hook::Binding&);
}
#endif

namespace scroller_png_hook {
using PatternCreate = void* (*)(void*);
inline constexpr ULONG_PTR iatRva = 0xC7BE0, originalRva = 0x34BA0;

struct Binding {
    ULONG_PTR frame{}, graphics{}, wbase{}, cairo{};
    scroller_png::CairoApi api;
    PatternCreate original{};
    const std::atomic<const ThemeProfile*>* profile{};
};
// Published before the IAT swap and immutable afterwards. The DLL and all
// referenced modules are pinned, including when the zone is later disabled.
inline Binding binding;
inline std::atomic<const Binding*> published{};
inline DWORD installation = ERROR_NOT_READY;

struct ThumbBinding {
    thumb_paint::Images images;
    thumb_png::CairoApi api;
};
// Prepared only when the separately controlled thumb zone is requested.
// Publishing it never changes the immutable static-PNG binding.
inline ThumbBinding thumbBinding;
inline std::atomic<const ThumbBinding*> publishedThumb{};
inline DWORD thumbPreparation = ERROR_NOT_READY;

inline void* patternForSurface(void* surface, const Binding& state,
                               const ThemeProfile* profile, const scroller_png::Identity* identity)
{
    const auto incomingError = GetLastError();
    void* copy{};
    if (identity && profile && (profile->zones & enableScrollerPng)) {
        const scroller_png::Palette colors{
            profile->colors[scrollerTrackBackground], profile->colors[scrollerArrowBackground],
            profile->colors[scrollerArrowGlyph], profile->colors[scrollerArrowHover],
            profile->colors[scrollerArrowPressed]
        };
        copy = scroller_png::clone(state.api, surface, *identity, colors);
    }
    SetLastError(incomingError);
    // Cairo retains the chosen surface. Release only our own clone reference,
    // after creating the pattern. The caller still owns its original surface.
    void* result = state.original(copy ? copy : surface);
    const auto outgoingError = GetLastError();
    if (copy) state.api.destroy(copy);
    SetLastError(outgoingError);
    return result;
}

inline void* patternForThumb(void* surface, const Binding& state, const ThumbBinding& thumb,
                            const ThemeProfile* profile, const ThumbSurfaceIdentity& id)
{
    const auto incomingError = GetLastError();
    void* copy{};
    if (profile && (profile->zones & enableScrollerThumb) && surface &&
        !state.api.status(surface) && !state.api.type(surface) &&
        thumb_paint::knownSurface(id, state.api.format(surface), state.api.width(surface), state.api.height(surface))) {
        const auto background = id.cacheState == 3 ? scrollerThumbPressed :
                                id.cacheState == 2 ? scrollerThumbHover : scrollerThumbBackground;
        copy = thumb_png::clone(thumb.api, surface,
            {profile->colors[background], profile->colors[scrollerThumbDetail]});
    }
    SetLastError(incomingError);
    void* result = state.original(copy ? copy : surface);
    const auto outgoingError = GetLastError();
    if (copy) state.api.destroy(copy);
    SetLastError(outgoingError);
    return result;
}

inline void* createPattern(void* surface)
{
    const auto caller = reinterpret_cast<ULONG_PTR>(_AddressOfReturnAddress());
    const auto state = published.load(std::memory_order_acquire);
#ifdef THEME_THUMB_SURFACE_PROBE
    // Pass the original caller's stack address before the observer adds frames.
    // This code is absent from the normal production build.
    const auto observerError = GetLastError();
    thumb_surface_capture::observe(caller, surface, *state);
    SetLastError(observerError);
#endif
    const auto profile = state->profile->load(std::memory_order_acquire);
    if (!(profile->zones & (enableScrollerPng | enableScrollerThumb))) return state->original(surface);
    const auto incomingError = GetLastError();
    scroller_png::Identity identity{};
    const bool matched = (profile->zones & enableScrollerPng) && scroller_png::identify(caller,
                                                state->frame, state->graphics, state->wbase, identity);
    if (matched) {
        SetLastError(incomingError);
        return patternForSurface(surface, *state, profile, &identity);
    }
    if (profile->zones & enableScrollerThumb) {
        const auto thumb = publishedThumb.load(std::memory_order_acquire);
        ThumbSurfaceIdentity id{};
        thumb_paint::CacheObservation cache;
        if (thumb && thumb_paint::locatePaint(caller, thumb->images, id, cache) == ThumbSurfaceStage::Accepted) {
            SetLastError(incomingError);
            return patternForThumb(surface, *state, *thumb, profile, id);
        }
    }
    SetLastError(incomingError);
    return state->original(surface);
}

inline bool imageRegion(ULONG_PTR address, ULONG_PTR image, SIZE_T length, DWORD protection)
{
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.Protect == protection &&
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) == image && address >= start &&
        address - start < region.RegionSize && length <= region.RegionSize - (address - start);
}

inline bool slotEquals(const Binding& state, PVOID expected)
{
    const auto address = state.graphics + iatRva;
    PVOID current{};
    return !(address % alignof(PVOID)) && imageRegion(address, state.graphics, sizeof(PVOID), PAGE_READONLY) &&
        panel_detail::copy(&current, address, sizeof(current)) && current == expected;
}

inline bool swap(PVOID volatile* slot, PVOID desired, PVOID expected, PVOID& previous)
{
    __try {
        previous = InterlockedCompareExchangePointer(slot, desired, expected);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR || GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

inline DWORD errorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

inline DWORD installSlot(const Binding& state)
{
    const auto original = reinterpret_cast<PVOID>(state.original);
    const auto wrapper = reinterpret_cast<PVOID>(&createPattern);
    if (!slotEquals(state, original)) return ERROR_INVALID_DATA;
    const auto address = reinterpret_cast<LPVOID>(state.graphics + iatRva);
    auto* slot = reinterpret_cast<PVOID volatile*>(address);
    DWORD previousProtection{};
    if (!VirtualProtect(address, sizeof(PVOID), PAGE_READWRITE, &previousProtection))
        return errorOr(ERROR_ACCESS_DENIED);
    DWORD error{};
    bool owned{};
    PVOID previous{};
    if (previousProtection != PAGE_READONLY) error = ERROR_INVALID_STATE;
    else if (!swap(slot, wrapper, original, previous)) error = ERROR_NOACCESS;
    else if (previous != original) error = ERROR_INVALID_DATA;
    else owned = true;
    DWORD ignored{};
    if (!VirtualProtect(address, sizeof(PVOID), previousProtection, &ignored)) {
        if (!error) error = errorOr(ERROR_ACCESS_DENIED);
        // The page is still writable. Revert only our own pointer, never a
        // competing hook. There is one bounded protection restoration retry.
        const bool rollbackUncertain = owned &&
            (!swap(slot, original, wrapper, previous) || previous != wrapper);
        const bool restored = VirtualProtect(address, sizeof(PVOID), previousProtection, &ignored) != FALSE;
        return restored && !rollbackUncertain && (!owned || slotEquals(state, original)) ? error : themeStateUncertain;
    }
    if (error) return error;
    return slotEquals(state, wrapper) ? ERROR_SUCCESS : themeStateUncertain;
}

template<typename Function>
inline bool resolve(HMODULE module, const char* name, Function& destination)
{
    const auto pointer = GetProcAddress(module, name);
    if (!pointer || !imageRegion(reinterpret_cast<ULONG_PTR>(pointer), reinterpret_cast<ULONG_PTR>(module),
                                  1, PAGE_EXECUTE_READ)) return false;
    destination = reinterpret_cast<Function>(pointer);
    return true;
}

inline DWORD prepare(ULONG_PTR frame, ULONG_PTR graphics, ULONG_PTR wbase,
                     const std::atomic<const ThemeProfile*>& profile, Binding& prepared)
{
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!cairo) return ERROR_MOD_NOT_FOUND;
    if (!frame || !graphics || !wbase || graphics > panel_detail::limit - iatRva ||
        reinterpret_cast<ULONG_PTR>(cairo) > panel_detail::limit - originalRva ||
        !matchesSha256(modulePath(cairo), cairoHash)) return ERROR_REVISION_MISMATCH;
    prepared = {frame, graphics, wbase, reinterpret_cast<ULONG_PTR>(cairo), {}, nullptr, &profile};
    auto& api = prepared.api;
    if (!resolve(cairo, "cairo_pattern_create_for_surface", prepared.original) ||
        reinterpret_cast<ULONG_PTR>(prepared.original) != prepared.cairo + originalRva ||
        !resolve(cairo, "cairo_surface_status", api.status) ||
        !resolve(cairo, "cairo_surface_get_type", api.type) ||
        !resolve(cairo, "cairo_image_surface_get_format", api.format) ||
        !resolve(cairo, "cairo_image_surface_get_width", api.width) ||
        !resolve(cairo, "cairo_image_surface_get_height", api.height) ||
        !resolve(cairo, "cairo_image_surface_get_stride", api.stride) ||
        !resolve(cairo, "cairo_surface_get_device_scale", api.scale) ||
        !resolve(cairo, "cairo_surface_get_device_offset", api.offset) ||
        !resolve(cairo, "cairo_surface_flush", api.flush) ||
        !resolve(cairo, "cairo_surface_destroy", api.destroy) ||
        !resolve(cairo, "cairo_surface_mark_dirty", api.markDirty) ||
        !resolve(cairo, "cairo_image_surface_get_data", api.data) ||
        !resolve(cairo, "cairo_image_surface_create", api.imageCreate)) return ERROR_PROC_NOT_FOUND;
    if (!slotEquals(prepared, reinterpret_cast<PVOID>(prepared.original))) return ERROR_INVALID_DATA;
    constexpr DWORD pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    const ULONG_PTR modules[]{reinterpret_cast<ULONG_PTR>(&createPattern), frame, graphics, wbase, prepared.cairo};
    for (const auto address : modules) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(address), &pinned)) return errorOr(ERROR_DLL_INIT_FAILED);
    }
    return ERROR_SUCCESS;
}

// Serialized by theme.cpp's installationLock. The hook stays installed while
// the process lives; the immutable profile controls pass-through immediately.
inline DWORD ensure(ULONG_PTR frame, ULONG_PTR graphics, ULONG_PTR wbase,
                    const std::atomic<const ThemeProfile*>& profile)
{
    if (installation == ERROR_SUCCESS)
        return slotEquals(binding, reinterpret_cast<PVOID>(&createPattern)) ? ERROR_SUCCESS : themeStateUncertain;
    if (installation != ERROR_NOT_READY && installation != ERROR_MOD_NOT_FOUND) return installation;
    Binding candidate;
    const auto error = prepare(frame, graphics, wbase, profile, candidate);
    if (error) return installation = error;
    binding = candidate;
    published.store(&binding, std::memory_order_release);
    return installation = installSlot(binding);
}

inline DWORD prepareThumb(const Binding& state, ThumbBinding& prepared)
{
    struct ModuleRef {
        HMODULE value{};
        ~ModuleRef() { if (value) FreeLibrary(value); }
        bool open(LPCWSTR name) { return GetModuleHandleExW(0, name, &value) != FALSE; }
    } imageUi, image;
    if (!imageUi.open(L"imageui.dll") || !image.open(L"image.dll")) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(imageUi.value), thumbSurfaceImageUiHash) ||
        !matchesSha256(modulePath(image.value), thumbSurfaceImageHash)) return ERROR_REVISION_MISMATCH;
    prepared.images = {state.frame, state.graphics, state.wbase, reinterpret_cast<ULONG_PTR>(imageUi.value)};
    prepared.api.surface = state.api;
    const auto cairo = reinterpret_cast<HMODULE>(state.cairo);
    if (!resolve(cairo, "cairo_surface_set_device_scale", prepared.api.setScale) ||
        !resolve(cairo, "cairo_surface_set_device_offset", prepared.api.setOffset)) return ERROR_PROC_NOT_FOUND;
    constexpr DWORD pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    for (const auto module : {imageUi.value, image.value}) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(module), &pinned)) return errorOr(ERROR_DLL_INIT_FAILED);
    }
    return ERROR_SUCCESS;
}

// Serialized by the same installationLock as ensure(). A failed optional
// preparation leaves the existing static-PNG hook usable and the old profile active.
inline DWORD ensureThumb()
{
    if (thumbPreparation == ERROR_SUCCESS) return ERROR_SUCCESS;
    if (thumbPreparation != ERROR_NOT_READY && thumbPreparation != ERROR_MOD_NOT_FOUND) return thumbPreparation;
    if (installation != ERROR_SUCCESS || published.load(std::memory_order_acquire) != &binding)
        return ERROR_NOT_READY;
    ThumbBinding candidate;
    const auto error = prepareThumb(binding, candidate);
    if (error) return thumbPreparation = error;
    thumbBinding = candidate;
    publishedThumb.store(&thumbBinding, std::memory_order_release);
    return thumbPreparation = ERROR_SUCCESS;
}
} // namespace scroller_png_hook
