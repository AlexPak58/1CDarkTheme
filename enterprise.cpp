// Enterprise Taxi: adapt only the native vector renderer, never bitmap/HTML content.
// The two IAT slots and text call chain are documented in upstream RESEARCH.md.
#include "platform.hpp"
#include <atomic>
#include <intrin.h>
#include <cmath>

namespace {
using Rgb = void (*)(void*, double, double, double);
using Rgba = void (*)(void*, double, double, double, double);
PVOID originalRgb{}, originalRgba{};
ULONG_PTR graphicsBase{};
ThemeProfile palette;
std::atomic<bool> active{};
SRWLOCK lock = SRWLOCK_INIT;
bool installed{};

bool channels(double r, double g, double b) {
    return r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1;
}
void unpack(DWORD rgb, double& r, double& g, double& b) {
    r = ((rgb >> 16) & 255) / 255.0;
    g = ((rgb >> 8) & 255) / 255.0;
    b = (rgb & 255) / 255.0;
}
// Exact standard neutral surfaces and Taxi's pale yellow chrome. Keep saturated
// status/selection colors and user images. Explicit neutral vector colors share
// this renderer and are also transformed (documented Enterprise limitation).
void surface(double& r, double& g, double& b) {
    if (!channels(r,g,b)) return;
    const double hi = (std::max)(r,(std::max)(g,b));
    const double lo = (std::min)(r,(std::min)(g,b));
    if (r > .92 && g > .65 && g < .91 && b > .10 && b < .40) unpack(palette.colors[propertySelection],r,g,b);
    else if (lo > .60 && hi > .80 && b > r + .08) unpack(palette.colors[propertySelection],r,g,b);
    else if (lo > .72 && hi - lo < .04) unpack(palette.colors[workspaceBackground],r,g,b);
    else if (r > .92 && g > .84 && b > .40 && b < .93 && r >= g)
        unpack(palette.colors[barsBackground],r,g,b);
}
void text(double& r, double& g, double& b) {
    if (!channels(r,g,b)) return;
    const double hi = (std::max)(r,(std::max)(g,b));
    const double lo = (std::min)(r,(std::min)(g,b));
    if (hi < .72 && hi - lo < .12) unpack(palette.colors[hi > .45 ? propertyMutedText : propertyText],r,g,b);
    // Standard dark blue links on native light forms.
    else if (b > r * 1.4 && b > g && r < .45 && g > .15 && b < .85)
        unpack(palette.colors[propertyGroupText],r,g,b);
}
void vectorColor(double& r, double& g, double& b) {
    const auto oldR=r, oldG=g, oldB=b;
    surface(r,g,b);
    if (r == oldR && g == oldG && b == oldB) text(r,g,b);
}
void hookedRgb(void* cr, double r, double g, double b) {
    if (active.load(std::memory_order_acquire)) surface(r,g,b);
    reinterpret_cast<Rgb>(originalRgb)(cr,r,g,b);
}
void hookedRgba(void* cr, double r, double g, double b, double a) {
    if (active.load(std::memory_order_acquire)) {
        const auto* stack = static_cast<const ULONG_PTR*>(_AddressOfReturnAddress());
        const auto top = reinterpret_cast<ULONG_PTR>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
        const auto address = reinterpret_cast<ULONG_PTR>(stack);
        // grphcs+2336 converts ARGB; its caller at stack+40 is the glyph path.
        const bool glyph = reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + 0x2336 &&
            address <= top && top - address >= 0x48 && stack[0x40 / 8] == graphicsBase + 0x78F51;
        if (glyph) text(r,g,b);
        else if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + 0xAEA59) surface(r,g,b);
        else vectorColor(r,g,b);
    }
    reinterpret_cast<Rgba>(originalRgba)(cr,r,g,b,a);
}
DWORD install() {
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!graphics || !cairo) return ERROR_MOD_NOT_FOUND;
    if ((!matchesSha256(modulePath(nullptr),exeHash) && !matchesSha256(modulePath(nullptr),thinClientHash)) ||
        !matchesSha256(modulePath(graphics),graphicsHash)) return ERROR_REVISION_MISMATCH;
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    auto* rgb = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7B60);
    auto* rgba = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7C20);
    originalRgb = reinterpret_cast<PVOID>(GetProcAddress(cairo,"cairo_set_source_rgb"));
    originalRgba = reinterpret_cast<PVOID>(GetProcAddress(cairo,"cairo_set_source_rgba"));
    if (!originalRgb || !originalRgba || *rgb != originalRgb || *rgba != originalRgba) return ERROR_INVALID_DATA;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&hookedRgb),&pinned)) return GetLastError();
    DWORD protection{};
    auto* start = const_cast<PVOID*>(rgb);
    constexpr SIZE_T size = 0xC7C20 - 0xC7B60 + sizeof(PVOID);
    if (!VirtualProtect(start,size,PAGE_READWRITE,&protection)) return GetLastError();
    const auto beforeRgb = InterlockedCompareExchangePointer(rgb,reinterpret_cast<PVOID>(&hookedRgb),originalRgb);
    const auto beforeRgba = InterlockedCompareExchangePointer(rgba,reinterpret_cast<PVOID>(&hookedRgba),originalRgba);
    DWORD error = ERROR_SUCCESS;
    if (beforeRgb != originalRgb || beforeRgba != originalRgba) {
        if (beforeRgb == originalRgb) InterlockedCompareExchangePointer(rgb,originalRgb,reinterpret_cast<PVOID>(&hookedRgb));
        if (beforeRgba == originalRgba) InterlockedCompareExchangePointer(rgba,originalRgba,reinterpret_cast<PVOID>(&hookedRgba));
        error = ERROR_INVALID_DATA;
    }
    DWORD ignored{};
    if (!VirtualProtect(start,size,protection,&ignored) && !error) error = GetLastError();
    installed = !error;
    return error;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI SetThemeProfile(LPVOID argument) {
    if (!argument) return ERROR_INVALID_PARAMETER;
    const auto& requested = *static_cast<const ThemeProfile*>(argument);
    if (requested.size != sizeof(ThemeProfile) || requested.version != themeProfileVersion || requested.zones > themeZoneMask)
        return ERROR_INVALID_PARAMETER;
    for (const auto color : requested.colors) if (color > 0xFFFFFF) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    DWORD result = ERROR_ALREADY_ASSIGNED;
    if (!active.load(std::memory_order_relaxed)) {
        result = installed ? ERROR_SUCCESS : install();
        if (!result) {
            palette = requested;
            active.store(requested.zones != 0,std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
