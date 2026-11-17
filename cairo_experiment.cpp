#include "platform.hpp"
#ifdef THEME_TOOLTIP_TRACE
#include "tooltip_trace.hpp"
#endif

#include <intrin.h>
#include <atomic>

namespace {

using SetSourceRgb = void (*)(void*, double, double, double);
using SetSourceRgba = void (*)(void*, double, double, double, double);
constexpr ULONG_PTR sourceRgbSlot = 0xC7B60;
constexpr ULONG_PTR sourceRgbaSlot = 0xC7C20;
constexpr ULONG_PTR solidFillReturn = 0xAEE56;
constexpr ULONG_PTR treeRowsReturn = 0x303466;
constexpr ULONG_PTR listPaintReturn = 0x8117F5;
constexpr ULONG_PTR treePaintVtable = 0xA88AC0;

struct ImportHook {
    ULONG_PTR offset;
    const char* name;
    PVOID replacement;
    PVOID original{};
    PVOID volatile* slot{};
};

void setSourceRgb(void*, double, double, double);
void setSourceRgba(void*, double, double, double, double);

ImportHook hooks[] = {
    {sourceRgbSlot, "cairo_set_source_rgb", reinterpret_cast<PVOID>(&setSourceRgb)},
    {sourceRgbaSlot, "cairo_set_source_rgba", reinterpret_cast<PVOID>(&setSourceRgba)},
};
#ifdef THEME_TOOLTIP_TRACE
constexpr size_t firstHook = 1;
constexpr size_t hookCount = std::size(hooks);
SRWLOCK traceLock = SRWLOCK_INIT;
TooltipTrace trace;
const TooltipTracePlatform* tracePlatform;
#else
constexpr size_t firstHook = 0;
#ifdef THEME_BACKGROUND_ONLY
constexpr size_t hookCount = 1;
#else
constexpr size_t hookCount = std::size(hooks);
#endif
#endif
ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
SRWLOCK controlLock = SRWLOCK_INIT;
std::atomic<bool> enabled;

bool isTreeReceiver(const ULONG_PTR* stack, size_t paintOffset)
{
    if (stack[paintOffset / sizeof(*stack)] != frameBase + listPaintReturn) return false;
    const auto savedRdi = stack[(paintOffset - 0x20) / sizeof(*stack)];
    if (savedRdi <= 0x100 || (savedRdi % alignof(ULONG_PTR))) return false;
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(savedRdi - 0x100) == frameBase + treePaintVtable;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool isTreeFill(const ULONG_PTR* stack)
{
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    if (address > stackTop - 0x278) return false;
    if (stack[0xA0 / sizeof(*stack)] == graphicsBase + 0xAC6DC &&
        stack[0x150 / sizeof(*stack)] == frameBase + 0xA4638) {
        return isTreeReceiver(stack, 0x270);
    }
    return address <= stackTop - 0x608 &&
           stack[0x220 / sizeof(*stack)] == frameBase + treeRowsReturn &&
           isTreeReceiver(stack, 0x600);
}

#ifdef THEME_TOOLTIP_TRACE
bool isExpandedTooltip(const ULONG_PTR* stack)
{
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    const auto& renderer = tracePlatform->rendererReturn;
    if (address > stackTop || stackTop - address < renderer.stackOffset + sizeof(*stack)) return false;
    // Walk the verified fixed stack frames back to frame.dll's tooltip renderer.
    for (const auto& entry : tracePlatform->graphicsReturns) {
        const auto actual = stack[entry.stackOffset / sizeof(*stack)];
        if (actual != graphicsBase + entry.rva &&
            (!entry.alternateRva || actual != graphicsBase + entry.alternateRva)) return false;
    }
    return stack[renderer.stackOffset / sizeof(*stack)] == frameBase + renderer.rva;
}

void countTooltipColor(double red, double green, double blue, double alpha)
{
    const auto isChannel = [](double value) { return value >= 0 && value <= 1; };
    if (!isChannel(red) || !isChannel(green) || !isChannel(blue) || !isChannel(alpha)) return;
    const auto byte = [](double value) { return static_cast<DWORD>(value * 255.0 + 0.5); };
    const DWORD rgba = (byte(red) << 24) | (byte(green) << 16) | (byte(blue) << 8) | byte(alpha);
    AcquireSRWLockExclusive(&traceLock);
    DWORD* count = &trace.otherCalls;
    for (auto& color : trace.colors) {
        if (color.calls && color.rgba != rgba) continue;
        color.rgba = rgba;
        count = &color.calls;
        break;
    }
    if (*count != MAXDWORD) ++*count;
    ReleaseSRWLockExclusive(&traceLock);
}
#endif

void setSourceRgb(void* context, double red, double green, double blue)
{
    // The verified grphcs function ends in a tail jump through this IAT slot.
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + solidFillReturn &&
        red == 1.0 && green == 1.0 && blue == 1.0 &&
        enabled.load(std::memory_order_relaxed) &&
        isTreeFill(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()))) {
        red = 0x1F / 255.0;
        green = 0x29 / 255.0;
        blue = 0x38 / 255.0;
    }
    reinterpret_cast<SetSourceRgb>(hooks[0].original)(context, red, green, blue);
}

void setSourceRgba(void* context, double red, double green, double blue, double alpha)
{
#ifdef THEME_TOOLTIP_TRACE
    const auto expectedReturn = tracePlatform->rgbaReturn;
#else
    // grphcs+0x22D0 calls Cairo with the original ARGB split into four channels.
    constexpr auto expectedReturn = 0x2336;
#endif
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + expectedReturn &&
        enabled.load(std::memory_order_relaxed)) {
        const auto* stack = static_cast<const ULONG_PTR*>(_AddressOfReturnAddress());
#ifdef THEME_TOOLTIP_TRACE
        if (isExpandedTooltip(stack)) countTooltipColor(red, green, blue, alpha);
#else
        const auto stackTop = reinterpret_cast<ULONG_PTR>(
            reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
        const auto remaining = stackTop - reinterpret_cast<ULONG_PTR>(stack);
        if (remaining >= 0xCC8 && stack[0x40 / 8] == graphicsBase + 0x7905B &&
            red == 1.0 && green == 1.0 && blue == 1.0 &&
            stack[0x4C0 / 8] == frameBase + 0x85F475 && isTreeReceiver(stack, 0xCC0)) {
            red = 0x1F / 255.0;
            green = 0x29 / 255.0;
            blue = 0x38 / 255.0;
        } else if (remaining >= 0x698 && stack[0x40 / 8] == graphicsBase + 0x79821 &&
                   red == 0.0 && green == 0.0 && blue == 0.0 &&
                   (stack[0x690 / 8] == frameBase + 0x85F2CD ||
                    (remaining >= 0xD88 && stack[0x580 / 8] == frameBase + 0x85F475 &&
                     isTreeReceiver(stack, 0xD80)))) {
            red = 0x80 / 255.0;
            green = 0xF2 / 255.0;
            blue = 0xC2 / 255.0;
        }
#endif
    }
    reinterpret_cast<SetSourceRgba>(hooks[1].original)(context, red, green, blue, alpha);
}

DWORD resolveHooks()
{
    const auto frame = GetModuleHandleW(L"frame.dll");
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!frame || !graphics || !cairo) return ERROR_MOD_NOT_FOUND;
#ifdef THEME_TOOLTIP_TRACE
    if (IsDebuggerPresent()) return ERROR_REVISION_MISMATCH;
    const auto* platform = findTooltipTracePlatform(modulePath(nullptr));
    if (!platform || !matchesSha256(modulePath(frame), platform->rendererHash) ||
        !matchesSha256(modulePath(graphics), platform->graphicsHash)) return ERROR_REVISION_MISMATCH;
    tracePlatform = platform;
    hooks[firstHook].offset = platform->rgbaSlot;
#else
    if (IsDebuggerPresent() || !matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash) ||
        !matchesSha256(modulePath(graphics), graphicsHash)) return ERROR_REVISION_MISMATCH;
#endif

    for (size_t i = firstHook; i < hookCount; ++i) {
        hooks[i].original = reinterpret_cast<PVOID>(GetProcAddress(cairo, hooks[i].name));
        if (!hooks[i].original) return ERROR_PROC_NOT_FOUND;
    }
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    HMODULE pinned{};
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&setSourceRgb), &pinned))
        return GetLastError();
    frameBase = reinterpret_cast<ULONG_PTR>(frame);
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    for (size_t i = firstHook; i < hookCount; ++i)
        hooks[i].slot = reinterpret_cast<PVOID volatile*>(graphicsBase + hooks[i].offset);
    return ERROR_SUCCESS;
}

DWORD setHook(bool on)
{
    if (!hooks[firstHook].slot) {
        if (!on) return ERROR_SUCCESS;
        const auto error = resolveHooks();
        if (error) return error;
    }
    if (!on) enabled.store(false, std::memory_order_relaxed);
    auto* address = const_cast<PVOID*>(hooks[firstHook].slot);
    const auto size = hooks[hookCount - 1].offset - hooks[firstHook].offset + sizeof(PVOID);
    DWORD protection{};
    if (!VirtualProtect(address, size, PAGE_READWRITE, &protection)) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    bool changed[hookCount]{};
    for (size_t i = firstHook; i < hookCount; ++i) {
        const auto& hook = hooks[i];
        const auto expected = on ? hook.original : hook.replacement;
        const auto replacement = on ? hook.replacement : hook.original;
        const auto previous = InterlockedCompareExchangePointer(hook.slot, replacement, expected);
        changed[i] = previous == expected;
        if (!changed[i] && previous != replacement) error = ERROR_INVALID_DATA;
    }
    DWORD ignored{};
    if (error && on) {
        for (size_t i = firstHook; i < hookCount; ++i) {
            const auto& hook = hooks[i];
            if (changed[i]) InterlockedCompareExchangePointer(hook.slot, hook.original, hook.replacement);
        }
    }
    if (!VirtualProtect(address, size, protection, &ignored) && !error) error = GetLastError();
    if (!error && on) enabled.store(true, std::memory_order_relaxed);
    return error;
}

} // namespace

#ifdef THEME_TOOLTIP_TRACE
extern "C" __declspec(dllexport) DWORD WINAPI ReadTooltipTrace(LPVOID output)
{
    if (!output) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&traceLock);
    *static_cast<TooltipTrace*>(output) = trace;
    trace = {};
    ReleaseSRWLockExclusive(&traceLock);
    return ERROR_SUCCESS;
}
#endif

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(LPVOID setting)
{
    if (setting && reinterpret_cast<ULONG_PTR>(setting) != enableTreeOnly) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&controlLock);
    const auto error = setHook(setting != nullptr);
    ReleaseSRWLockExclusive(&controlLock);
    return error;
}
