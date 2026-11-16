#include "platform.hpp"

#include <intrin.h>

namespace {

using SetSourceRgb = void (*)(void*, double, double, double);
constexpr ULONG_PTR sourceRgbSlot = 0xC7B60;
constexpr ULONG_PTR solidFillReturn = 0xAEE56;
constexpr ULONG_PTR treeRowsReturn = 0x303466;
constexpr ULONG_PTR listPaintReturn = 0x8117F5;
constexpr ULONG_PTR treePaintVtable = 0xA88AC0;

SetSourceRgb original;
PVOID volatile* slot;
ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
SRWLOCK controlLock = SRWLOCK_INIT;

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

void setSourceRgb(void* context, double red, double green, double blue)
{
    // The verified grphcs function ends in a tail jump through this IAT slot.
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + solidFillReturn &&
        red == 1.0 && green == 1.0 && blue == 1.0 &&
        isTreeFill(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()))) {
        red = 0x1F / 255.0;
        green = 0x29 / 255.0;
        blue = 0x38 / 255.0;
    }
    original(context, red, green, blue);
}

DWORD resolveSlot()
{
    const auto frame = GetModuleHandleW(L"frame.dll");
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!frame || !graphics || !cairo) return ERROR_MOD_NOT_FOUND;
    if (IsDebuggerPresent() || !matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash) ||
        !matchesSha256(modulePath(graphics), graphicsHash)) return ERROR_REVISION_MISMATCH;

    original = reinterpret_cast<SetSourceRgb>(GetProcAddress(cairo, "cairo_set_source_rgb"));
    if (!original) return ERROR_PROC_NOT_FOUND;
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    HMODULE pinned{};
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&setSourceRgb), &pinned))
        return GetLastError();
    frameBase = reinterpret_cast<ULONG_PTR>(frame);
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    slot = reinterpret_cast<PVOID volatile*>(graphicsBase + sourceRgbSlot);
    return ERROR_SUCCESS;
}

DWORD setHook(bool on)
{
    if (!slot) {
        if (!on) return ERROR_SUCCESS;
        const auto error = resolveSlot();
        if (error) return error;
    }
    const auto native = reinterpret_cast<PVOID>(original);
    const auto hook = reinterpret_cast<PVOID>(&setSourceRgb);
    auto* address = const_cast<PVOID*>(slot);
    DWORD protection{};
    if (!VirtualProtect(address, sizeof(PVOID), PAGE_READWRITE, &protection)) return GetLastError();
    const auto expected = on ? native : hook;
    const auto replacement = on ? hook : native;
    const auto previous = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored{};
    if (!VirtualProtect(address, sizeof(PVOID), protection, &ignored)) {
        const auto error = GetLastError();
        if (on && previous == native) InterlockedCompareExchangePointer(slot, native, hook);
        return error;
    }
    return previous == expected || previous == replacement ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(LPVOID setting)
{
    if (setting && reinterpret_cast<ULONG_PTR>(setting) != enableTreeOnly) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&controlLock);
    const auto error = setHook(setting != nullptr);
    ReleaseSRWLockExclusive(&controlLock);
    return error;
}
