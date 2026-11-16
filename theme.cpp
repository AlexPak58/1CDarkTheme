#include "platform.hpp"

#include <atomic>
#include <cstring>
#include <dwmapi.h>

static_assert(sizeof(void*) == 8);

namespace {

constexpr DWORD rgbMask = 0x00FFFFFF;
constexpr DWORD white = rgbMask;
constexpr DWORD backgroundRgb = 0x001F2938;
constexpr DWORD backgroundBgr = 0x0038291F;
constexpr DWORD workspaceBackgroundBgr = 0x00BDD3D7;
constexpr DWORD textRgb = 0x0080F2C2;
constexpr DWORD barBackgroundBgr = 0x00DDF2F5;
constexpr DWORD platformTextArgb = 0xFF413003;
constexpr DWORD platformMutedTextArgb = 0xFFA08B54;
constexpr DWORD chromeTextRgb = 0x00E5EBF5;
constexpr DWORD propertyMutedTextRgb = 0x00A7B5C7;
constexpr DWORD propertyGroupTextRgb = 0x00B9D4FF;
constexpr DWORD propertySelectionBgr = 0x008A5A30;
constexpr ULONG_PTR propertyPaintReturn = 0x4F60ED;
constexpr ULONG_PTR sharedListPaintReturn = 0x8117F5;
constexpr ULONG_PTR dropdownPaintVtable = 0xA8A278;
// Windows 11 DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR, also buildable with older SDKs.
constexpr DWORD captionColorAttribute = 35;
constexpr DWORD captionTextColorAttribute = 36;
constexpr COLORREF defaultDwmColor = 0xFFFFFFFF;

struct Site {
    BYTE* address;
    std::array<BYTE, 3> instruction;
    DWORD protection;
};

enum SiteIndex { rowFill, emptyFill, labelColor, workspaceFill };
Site sites[] = {
    {nullptr, {0x44, 0x8B, 0xC6}, 0}, // mov r8d, esi
    {nullptr, {0x0F, 0x45, 0xD0}, 0}, // cmovne edx, eax
    {nullptr, {0x41, 0x8B, 0xC0}, 0}, // mov eax, r8d
    {nullptr, {0x44, 0x8B, 0xC0}, 0}, // mov r8d, eax
};

ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
SRWLOCK installationLock = SRWLOCK_INIT;
std::atomic<ULONG_PTR> activeTheme{0};
bool ready;

HWND captionWindow;

BOOL CALLBACK findCaptionWindow(HWND window, LPARAM param)
{
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;

    auto& candidate = *reinterpret_cast<HWND*>(param);
    if (candidate) {
        candidate = nullptr;
        return FALSE;
    }
    candidate = window;
    return TRUE;
}

HRESULT setCaptionColors(HWND window, COLORREF background, COLORREF text)
{
    const HRESULT fill = DwmSetWindowAttribute(window, captionColorAttribute,
                                               &background, sizeof(background));
    if (FAILED(fill)) return fill;
    return DwmSetWindowAttribute(window, captionTextColorAttribute, &text, sizeof(text));
}

HRESULT restoreCaption()
{
    if (!captionWindow) return S_OK;
    DWORD pid{};
    GetWindowThreadProcessId(captionWindow, &pid);
    if (pid != GetCurrentProcessId()) {
        captionWindow = nullptr;
        return S_OK;
    }

    const HRESULT result = setCaptionColors(captionWindow, defaultDwmColor, defaultDwmColor);
    if (SUCCEEDED(result)) captionWindow = nullptr;
    return result;
}

HRESULT enableCaption()
{
    HWND window{};
    if (!EnumWindows(findCaptionWindow, reinterpret_cast<LPARAM>(&window)) || !window)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (captionWindow != window) {
        const HRESULT restored = restoreCaption();
        if (FAILED(restored)) return restored;
    }
    const COLORREF background = backgroundBgr;
    HRESULT result = DwmSetWindowAttribute(window, captionColorAttribute,
                                           &background, sizeof(background));
    if (FAILED(result)) return result;
    captionWindow = window;
    const COLORREF text = RGB(0xE5, 0xEB, 0xF5);
    result = DwmSetWindowAttribute(window, captionTextColorAttribute, &text, sizeof(text));
    if (FAILED(result)) restoreCaption();
    return result;
}

DWORD replaceRgb(DWORD value, DWORD rgb)
{
    return (value & ~rgbMask) | rgb;
}

ULONG_PTR callerAt(const CONTEXT& registers, ULONG_PTR offset)
{
    const auto* stack = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto location = registers.Rsp + offset;
    const auto top = reinterpret_cast<ULONG_PTR>(stack->StackBase);
    if (location < reinterpret_cast<ULONG_PTR>(stack->StackLimit) ||
        location > top - sizeof(ULONG_PTR)) return 0;
    return *reinterpret_cast<const ULONG_PTR*>(location);
}

bool isDropdownPaint(const CONTEXT& registers, ULONG_PTR paintReturnSlot)
{
    if (callerAt(registers, paintReturnSlot) != frameBase + sharedListPaintReturn)
        return false;

    // The paint prologue saves the dispatcher's RDI (receiver + 0x100).
    const auto savedRdi = callerAt(registers, paintReturnSlot - 0x20);
    if (savedRdi <= 0x100) return false;
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(savedRdi - 0x100) ==
               frameBase + dropdownPaintVtable;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

DWORD propertyFillColor(const CONTEXT& registers, DWORD bgr)
{
    const auto caller = callerAt(registers, 0x218);
    const bool background = bgr == white &&
        (caller == frameBase + 0x4F9AA5 || caller == frameBase + 0x4FA197);
    const bool selected = bgr == 0x00C26A53 && caller == frameBase + 0x4F9C51;
    if ((background || selected) &&
        callerAt(registers, 0x538) == frameBase + propertyPaintReturn)
        return selected ? propertySelectionBgr : backgroundBgr;
    return bgr;
}

DWORD propertyLabelColor(const CONTEXT& registers, DWORD argb)
{
    // The underlined value is drawn by a separate window from the property grid.
    if ((argb == platformTextArgb || argb == platformMutedTextArgb) &&
        callerAt(registers, 0x578) == frameBase + 0x5BD070 &&
        callerAt(registers, 0x608) == frameBase + 0x5B9393 &&
        callerAt(registers, 0xA78) == frameBase + 0x5B5C5D)
        return replaceRgb(argb, argb == platformTextArgb ? chromeTextRgb : propertyMutedTextRgb);

    // The search renderer draws the matching and ordinary fragments separately.
    if (argb == platformTextArgb &&
        callerAt(registers, 0x688) == frameBase + 0x4F9FF9 &&
        callerAt(registers, 0x9A8) == frameBase + propertyPaintReturn)
        return replaceRgb(argb, chromeTextRgb);

    const auto caller = callerAt(registers, 0x4F8);
    if ((caller != frameBase + 0x4F9E04 && caller != frameBase + 0x4FA40D) ||
        callerAt(registers, 0x818) != frameBase + propertyPaintReturn) return argb;

    if (caller == frameBase + 0x4FA40D && argb == 0xFF594304)
        return replaceRgb(argb, propertyGroupTextRgb);
    if (caller == frameBase + 0x4F9E04) {
        if (argb == platformTextArgb) return replaceRgb(argb, chromeTextRgb);
        if (argb == platformMutedTextArgb) return replaceRgb(argb, propertyMutedTextRgb);
    }
    return argb;
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto& event = *exception->ExceptionRecord;
    if (event.ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;

    auto& regs = *exception->ContextRecord;
    const ULONG_PTR theme = activeTheme.load(std::memory_order_relaxed);
    const bool dark = theme != 0;
    const bool withBars = (theme & enableTreeAndBars) != 0;
    const bool withProperties = (theme & enableTreeAndProperties) != 0;
    if (event.ExceptionAddress == sites[rowFill].address) {
        // This call site fills the area occupied by the tree's rows.
        const auto original = static_cast<DWORD>(regs.Rsi);
        regs.R8 = dark && (original & rgbMask) == white &&
                  !isDropdownPaint(regs, 0x3D8) ?
            replaceRgb(original, backgroundBgr) : original;
    } else if (event.ExceptionAddress == sites[emptyFill].address) {
        // CMOVNE selects the low 32 bits and clears the high 32 bits.
        regs.Rdx = static_cast<DWORD>((regs.EFlags & 0x40) == 0 ? regs.Rax : regs.Rdx);
        if (dark && static_cast<DWORD>(regs.Rdx) == white &&
            callerAt(regs, 0x98) == graphicsBase + 0xAC6DC &&
            callerAt(regs, 0x148) == frameBase + 0xA4638 &&
            !isDropdownPaint(regs, 0x268)) {
            regs.Rdx = backgroundBgr;
        } else if (withBars && (regs.EFlags & 0x40) == 0 && regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x218) == frameBase + 0x71A6DF &&
                   callerAt(regs, 0x2E8) == graphicsBase + 0xADAE5) {
            // The status bar shares the frame fill caller, but not this stack path.
            regs.Rdx = backgroundBgr;
        } else if (withProperties && (regs.EFlags & 0x40) == 0) {
            regs.Rdx = propertyFillColor(regs, static_cast<DWORD>(regs.Rdx));
        }
    } else if (event.ExceptionAddress == sites[labelColor].address) {
        auto argb = static_cast<DWORD>(regs.R8);
        const auto caller = callerAt(regs, 0x38);
        if (dark && (argb & rgbMask) == white && caller == graphicsBase + 0x7905B &&
            callerAt(regs, 0x4B8) == frameBase + 0x85F475 &&
            !isDropdownPaint(regs, 0xCB8)) {
            argb = replaceRgb(argb, backgroundRgb);
        } else if (dark && (argb & rgbMask) == 0 && caller == graphicsBase + 0x79821 &&
                   ((callerAt(regs, 0x578) == frameBase + 0x85F475 &&
                     !isDropdownPaint(regs, 0xD78)) ||
                    callerAt(regs, 0x688) == frameBase + 0x85F2CD)) {
            // The second caller draws separate fragments of filtered tree labels.
            argb = replaceRgb(argb, textRgb);
        } else if (withBars && argb == platformTextArgb && caller == graphicsBase + 0x79821 &&
                   callerAt(regs, 0x578) == frameBase + 0x73480F &&
                   callerAt(regs, 0x748) == frameBase + 0x739499 &&
                   callerAt(regs, 0x8A8) == frameBase + 0x70FC9C &&
                   callerAt(regs, 0xB68) == frameBase + 0x5091F) {
            // The popup shares the renderer through frame+0x50CAE, then diverges.
            argb = replaceRgb(argb, chromeTextRgb);
        } else if (withProperties && caller == graphicsBase + 0x79821) {
            argb = propertyLabelColor(regs, argb);
        }
        regs.R8 = argb;
        regs.Rax = argb;
    } else if (event.ExceptionAddress == sites[workspaceFill].address) {
        // Only the empty workspace's standard fill. Preserve other platform colors.
        const auto original = static_cast<DWORD>(regs.Rax);
        regs.R8 = (theme & enableWorkspace) && original == workspaceBackgroundBgr ?
            backgroundBgr : original;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    regs.Rip = reinterpret_cast<ULONG_PTR>(event.ExceptionAddress) + 3;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD installSites()
{
    const auto frame = GetModuleHandleW(L"frame.dll");
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    if (!frame || !graphics) return ERROR_MOD_NOT_FOUND;
    if (IsDebuggerPresent() || !matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash) ||
        !matchesSha256(modulePath(graphics), graphicsHash)) return ERROR_REVISION_MISMATCH;

    frameBase = reinterpret_cast<ULONG_PTR>(frame);
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    sites[rowFill].address = reinterpret_cast<BYTE*>(frameBase + 0x303454);
    sites[emptyFill].address = reinterpret_cast<BYTE*>(graphicsBase + 0xAEE4E);
    sites[labelColor].address = reinterpret_cast<BYTE*>(graphicsBase + 0x22D7);
    sites[workspaceFill].address = reinterpret_cast<BYTE*>(frameBase + 0x9518EC);
    for (const auto& site : sites) {
        if (std::memcmp(site.address, site.instruction.data(), site.instruction.size()) != 0)
            return ERROR_INVALID_DATA;
    }

    // The process owns the handler and these module addresses until exit.
    const LPCWSTR modules[] = {
        reinterpret_cast<LPCWSTR>(&onBreakpoint),
        reinterpret_cast<LPCWSTR>(frame),
        reinterpret_cast<LPCWSTR>(graphics),
    };
    constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    for (const auto& module : modules) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(flags, module, &pinned)) return GetLastError();
    }

    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    if (!handler) return ERROR_INVALID_FUNCTION;
    size_t prepared = 0;
    DWORD error = ERROR_SUCCESS;
    for (auto& site : sites) {
        if (!VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &site.protection)) {
            error = GetLastError();
            break;
        }
        ++prepared;
    }
    if (!error) {
        for (auto& site : sites) {
            InterlockedExchange8(reinterpret_cast<volatile char*>(site.address),
                                 static_cast<char>(0xCC));
            if (!FlushInstructionCache(GetCurrentProcess(), site.address, 1)) {
                error = GetLastError();
                break;
            }
        }
        if (error) {
            for (auto& site : sites) {
                InterlockedExchange8(reinterpret_cast<volatile char*>(site.address),
                                     static_cast<char>(site.instruction.front()));
                FlushInstructionCache(GetCurrentProcess(), site.address, 1);
            }
        }
    }
    for (size_t i = 0; i < prepared; ++i) {
        DWORD ignored{};
        if (!VirtualProtect(sites[i].address, 1, sites[i].protection, &ignored) && !error)
            error = GetLastError();
    }
    // A thread may still be returning from a breakpoint after rollback.
    // Keep the handler and addresses valid for the lifetime of this session.
    return error;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(LPVOID setting)
{
    AcquireSRWLockExclusive(&installationLock);
    static DWORD result = ERROR_NOT_READY;
    if (setting && result == ERROR_NOT_READY) {
        result = installSites();
        ready = result == ERROR_SUCCESS;
    }
    const auto requested = reinterpret_cast<ULONG_PTR>(setting);
    const bool withCaption = (requested & enableTreeAndCaption) != 0;
    const HRESULT captionResult = ready && withCaption ? enableCaption() : restoreCaption();
    activeTheme.store(ready && SUCCEEDED(captionResult) ? requested : 0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&installationLock);
    return FAILED(captionResult) ? static_cast<DWORD>(captionResult) : result;
}
