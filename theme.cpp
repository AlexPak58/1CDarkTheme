#include "platform.hpp"

#include <atomic>
#include <cstring>
#include <dwmapi.h>

static_assert(sizeof(void*) == 8);

namespace {

constexpr DWORD rgbMask = 0x00FFFFFF;
constexpr DWORD white = rgbMask;
constexpr DWORD workspaceBackgroundBgr = 0x00BDD3D7;
constexpr DWORD barBackgroundBgr = 0x00DDF2F5;
constexpr DWORD platformPopupMenuBackgroundBgr = 0x00EBFAFC;
constexpr DWORD platformPopupMenuGutterBgr = 0x00CCE6EB;
constexpr DWORD platformPopupShortcutArgb = 0xFFB38000;
constexpr ULONG_PTR popupMenuPaintReturn = 0x51331;
constexpr DWORD platformTextArgb = 0xFF413003;
constexpr DWORD platformMutedTextArgb = 0xFFA08B54;
constexpr DWORD statusMutedTextArgb = 0xFF9B8549;
constexpr DWORD platformActiveWindowTabBgr = 0x00E6CCB2;
constexpr DWORD platformInactiveWindowTabBgr = 0x00B2B2B2;
constexpr DWORD platformActiveTreeTabBgr = 0x00EBFAFC;
constexpr DWORD platformInactiveTreeTabBgr = 0x00CCE6EB;
constexpr DWORD platformTreeTabTextBgr = 0x00033041;
constexpr ULONG_PTR treeTabVtable = 0xAD3D38;
constexpr ULONG_PTR windowTabPaintReturn = 0x8EFC3A;
constexpr ULONG_PTR windowPanelPaintReturn = 0x8EF5DD;
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
    BYTE length{3};
};

enum SiteIndex { rowFill, emptyFill, labelColor, workspaceFill, treeTabFill, treeTabText, toolbarEdgeStroke };
Site sites[] = {
    {nullptr, {0x44, 0x8B, 0xC6}, 0}, // mov r8d, esi
    {nullptr, {0x0F, 0x45, 0xD0}, 0}, // cmovne edx, eax
    {nullptr, {0x41, 0x8B, 0xC0}, 0}, // mov eax, r8d
    {nullptr, {0x44, 0x8B, 0xC0}, 0}, // mov r8d, eax
    {nullptr, {0x8B, 0xD8}, 0, 2},  // mov ebx, eax
    {nullptr, {0x44, 0x8B, 0xC0}, 0}, // mov r8d, eax
    {nullptr, {0x8B, 0xD7}, 0, 2},  // mov edx, edi
};
#ifdef THEME_BACKGROUND_ONLY
constexpr size_t installedSiteCount = 2;
#else
constexpr size_t installedSiteCount = std::size(sites);
#endif

ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
SRWLOCK installationLock = SRWLOCK_INIT;
constexpr ThemeProfile defaultProfile{};
ThemeProfile customProfile;
std::atomic<const ThemeProfile*> activeProfile{&defaultProfile};
std::atomic<ULONG_PTR> activeTheme{0};
DWORD installationResult = ERROR_NOT_READY;
bool customApplied;

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

HRESULT enableCaption(const ThemeProfile& profile)
{
    HWND window{};
    if (!EnumWindows(findCaptionWindow, reinterpret_cast<LPARAM>(&window)) || !window)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (captionWindow != window) {
        const HRESULT restored = restoreCaption();
        if (FAILED(restored)) return restored;
    }
    const COLORREF background = toBgr(profile.colors[captionBackground]);
    HRESULT result = DwmSetWindowAttribute(window, captionColorAttribute,
                                           &background, sizeof(background));
    if (FAILED(result)) return result;
    captionWindow = window;
    const COLORREF text = toBgr(profile.colors[captionText]);
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

// The trees and this popup use a shared renderer with distinct receivers.
DWORD listFill(const CONTEXT& regs, ULONG_PTR slot, DWORD original, ULONG_PTR zones,
               const ThemeProfile& palette, bool useBgr)
{
    if (!(zones & (enableTreeOnly | enableDropdown))) return original;
    const bool dropdown = isDropdownPaint(regs, slot);
    if (!(zones & (dropdown ? enableDropdown : enableTreeOnly))) return original;
    const DWORD fill = palette.colors[dropdown ? dropdownBackground : treeBackground];
    return replaceRgb(original, useBgr ? toBgr(fill) : fill);
}

bool isTreeTabStrip(ULONG_PTR receiver)
{
    // Docked tab strip in the layout observed on both BPDemo trees.
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(receiver) == frameBase + treeTabVtable &&
               *reinterpret_cast<const DWORD*>(receiver + 0x10C) == 2;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

DWORD propertyFillColor(const CONTEXT& registers, DWORD bgr, const ThemeProfile& profile)
{
    const auto caller = callerAt(registers, 0x218);
    const bool background = bgr == white &&
        (caller == frameBase + 0x4F9AA5 || caller == frameBase + 0x4FA197);
    const bool selected = bgr == 0x00C26A53 && caller == frameBase + 0x4F9C51;
    if ((background || selected) &&
        callerAt(registers, 0x538) == frameBase + propertyPaintReturn)
        return toBgr(profile.colors[selected ? propertySelection : propertyBackground]);
    return bgr;
}

DWORD propertyLabelColor(const CONTEXT& registers, DWORD argb, const ThemeProfile& profile)
{
    // The underlined value is drawn by a separate window from the property grid.
    if ((argb == platformTextArgb || argb == platformMutedTextArgb) &&
        callerAt(registers, 0x578) == frameBase + 0x5BD070 &&
        callerAt(registers, 0x608) == frameBase + 0x5B9393 &&
        callerAt(registers, 0xA78) == frameBase + 0x5B5C5D)
        return replaceRgb(argb, profile.colors[argb == platformTextArgb ? propertyText : propertyMutedText]);

    // The search renderer draws the matching and ordinary fragments separately.
    if (argb == platformTextArgb &&
        callerAt(registers, 0x688) == frameBase + 0x4F9FF9 &&
        callerAt(registers, 0x9A8) == frameBase + propertyPaintReturn)
        return replaceRgb(argb, profile.colors[propertyText]);

    const auto caller = callerAt(registers, 0x4F8);
    if ((caller != frameBase + 0x4F9E04 && caller != frameBase + 0x4FA40D) ||
        callerAt(registers, 0x818) != frameBase + propertyPaintReturn) return argb;

    if (caller == frameBase + 0x4FA40D && argb == 0xFF594304)
        return replaceRgb(argb, profile.colors[propertyGroupText]);
    if (caller == frameBase + 0x4F9E04) {
        if (argb == platformTextArgb) return replaceRgb(argb, profile.colors[propertyText]);
        if (argb == platformMutedTextArgb) return replaceRgb(argb, profile.colors[propertyMutedText]);
    }
    return argb;
}

bool isStatusBarText(const CONTEXT& registers)
{
    if (callerAt(registers, 0x578) != frameBase + 0x73480F) return false;

    // The message uses a different text renderer with a larger stack frame.
    const auto renderer = callerAt(registers, 0x748);
    const bool message = renderer == frameBase + 0x73B4D7;
    if (!message && renderer != frameBase + 0x739499 && renderer != frameBase + 0x7397A8)
        return false;

    const ULONG_PTR shift = message ? 0x10 : 0;
    return callerAt(registers, 0x8A8 + shift) == frameBase + 0x72A366 &&
           callerAt(registers, 0x9F8 + shift) == frameBase + 0x70FBA5 &&
           callerAt(registers, 0xBB8 + shift) == frameBase + 0x73213F;
}

DWORD popupMenuFillColor(const CONTEXT& registers, DWORD bgr, const ThemeProfile& palette)
{
    if (callerAt(registers, 0x278) != frameBase + 0x70FB7B ||
        callerAt(registers, 0x328) != frameBase + 0x50CAE ||
        callerAt(registers, 0x438) != frameBase + popupMenuPaintReturn)
        return bgr;

    const auto painter = callerAt(registers, 0x218);
    if (bgr == platformPopupMenuBackgroundBgr && painter == frameBase + 0x725205)
        return toBgr(palette.colors[popupMenuBackground]);
    if (bgr == platformPopupMenuGutterBgr && painter == frameBase + 0x7252B5)
        return toBgr(palette.colors[popupMenuGutter]);
    return bgr;
}

DWORD popupMenuLabelColor(const CONTEXT& registers, DWORD argb, const ThemeProfile& palette)
{
    const bool shortcut = argb == platformPopupShortcutArgb;
    if ((!shortcut && argb != platformTextArgb) ||
        callerAt(registers, 0xB68) != frameBase + popupMenuPaintReturn ||
        callerAt(registers, 0x8A8) != frameBase + 0x70FC9C ||
        callerAt(registers, 0x748) != frameBase + 0x739499 ||
        callerAt(registers, 0x578) != frameBase + (shortcut ? 0x73499F : 0x73480F))
        return argb;
    return replaceRgb(argb, palette.colors[shortcut ? popupMenuShortcut : popupMenuText]);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto& event = *exception->ExceptionRecord;
    if (event.ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;

    auto& regs = *exception->ContextRecord;
    const ULONG_PTR theme = activeTheme.load(std::memory_order_acquire);
    const auto& colors = *activeProfile.load(std::memory_order_relaxed);
    const bool dark = (theme & enableTreeOnly) != 0;
    const bool withLists = (theme & (enableTreeOnly | enableDropdown)) != 0;
    const bool withBars = (theme & enableTreeAndBars) != 0;
    const bool withProperties = (theme & enableTreeAndProperties) != 0;
    const bool withStatusBar = (theme & enableStatusBar) != 0;
    const bool withWindowTabs = (theme & enableWindowTabs) != 0;
    const bool withTreeTabs = (theme & enableTreeTabs) != 0;
    const bool withPopupMenu = (theme & enablePopupMenu) != 0;
    BYTE length = 3;
    if (event.ExceptionAddress == sites[rowFill].address) {
        // This call site fills the area occupied by the tree's rows.
        const auto original = static_cast<DWORD>(regs.Rsi);
        regs.R8 = (original & rgbMask) == white ?
            listFill(regs, 0x3D8, original, theme, colors, true) : original;
    } else if (event.ExceptionAddress == sites[emptyFill].address) {
        // CMOVNE selects the low 32 bits and clears the high 32 bits.
        regs.Rdx = static_cast<DWORD>((regs.EFlags & 0x40) == 0 ? regs.Rax : regs.Rdx);
        if (withLists && static_cast<DWORD>(regs.Rdx) == white &&
            callerAt(regs, 0x98) == graphicsBase + 0xAC6DC &&
            callerAt(regs, 0x148) == frameBase + 0xA4638) {
            regs.Rdx = listFill(regs, 0x268, white, theme, colors, true);
        } else if ((theme & enableToolbarBand) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x98) == graphicsBase + 0xAC6DC &&
                   callerAt(regs, 0x148) == frameBase + 0x8E9D27) {
            regs.Rdx = toBgr(colors.colors[toolbarBandBackground]);
        } else if ((theme & enableToolbarBorder) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x98) == graphicsBase + 0xB89BF &&
                   callerAt(regs, 0x1E8) == frameBase + 0x8BD1DD &&
                   callerAt(regs, 0x2A8) == frameBase + 0x8BB658) {
            regs.Rdx = toBgr(colors.colors[toolbarBorder]);
        } else if ((withBars || withStatusBar || withWindowTabs) && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x218) == frameBase + 0x71A6DF) {
            const bool toolbar = withBars && callerAt(regs, 0x2E8) == graphicsBase + 0xADAE5;
            const bool status = withStatusBar &&
                callerAt(regs, 0x2B8) == frameBase + 0x70FB7B &&
                callerAt(regs, 0x478) == frameBase + 0x73213F;
            const bool windowPanel = withWindowTabs &&
                callerAt(regs, 0x2B8) == frameBase + 0x70FB7B &&
                callerAt(regs, 0x478) == frameBase + windowPanelPaintReturn;
            if (toolbar) regs.Rdx = toBgr(colors.colors[barsBackground]);
            if (status) regs.Rdx = toBgr(colors.colors[statusBackground]);
            if (windowPanel) regs.Rdx = toBgr(colors.colors[windowTabsBackground]);
        } else if (withWindowTabs && (regs.EFlags & 0x40) == 0 &&
                   (regs.Rdx == platformActiveWindowTabBgr ||
                    regs.Rdx == platformInactiveWindowTabBgr) &&
                   callerAt(regs, 0x158) == frameBase + 0x733FCF &&
                   callerAt(regs, 0x328) == frameBase + 0x739629 &&
                   callerAt(regs, 0x588) == frameBase + windowTabPaintReturn) {
            regs.Rdx = regs.Rdx == platformActiveWindowTabBgr ?
                toBgr(colors.colors[windowTabsActive]) : toBgr(colors.colors[windowTabsInactive]);
        } else if (withTreeTabs && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == platformActiveTreeTabBgr &&
                   callerAt(regs, 0x148) == frameBase + 0x5D33D5 &&
                   callerAt(regs, 0x1F8) == frameBase + 0x5CE349) {
            regs.Rdx = toBgr(colors.colors[treeTabsBackground]);
        } else if (withProperties && (regs.EFlags & 0x40) == 0) {
            regs.Rdx = propertyFillColor(regs, static_cast<DWORD>(regs.Rdx), colors);
        }
        if (withPopupMenu && (regs.EFlags & 0x40) == 0 &&
            (regs.Rdx == platformPopupMenuBackgroundBgr || regs.Rdx == platformPopupMenuGutterBgr))
            regs.Rdx = popupMenuFillColor(regs, static_cast<DWORD>(regs.Rdx), colors);
    } else if (event.ExceptionAddress == sites[labelColor].address) {
        auto argb = static_cast<DWORD>(regs.R8);
        const auto caller = callerAt(regs, 0x38);
        if (withLists && (argb & rgbMask) == white && caller == graphicsBase + 0x7905B &&
            callerAt(regs, 0x4B8) == frameBase + 0x85F475) {
            argb = listFill(regs, 0xCB8, argb, theme, colors, false);
        } else if (withLists && (argb & rgbMask) == 0 && caller == graphicsBase + 0x79821) {
            if (callerAt(regs, 0x578) == frameBase + 0x85F475) {
                const bool dropdown = isDropdownPaint(regs, 0xD78);
                if (theme & (dropdown ? enableDropdown : enableTreeOnly))
                    argb = replaceRgb(argb, colors.colors[dropdown ? dropdownText : treeText]);
            } else if (dark && callerAt(regs, 0x688) == frameBase + 0x85F2CD) {
                // Separate fragments of filtered tree labels.
                argb = replaceRgb(argb, colors.colors[treeText]);
            }
        } else if (withBars && argb == platformTextArgb && caller == graphicsBase + 0x79821 &&
                   callerAt(regs, 0x578) == frameBase + 0x73480F &&
                   callerAt(regs, 0x748) == frameBase + 0x739499 &&
                   callerAt(regs, 0x8A8) == frameBase + 0x70FC9C &&
                   callerAt(regs, 0xB68) == frameBase + 0x5091F) {
            // The popup shares the renderer through frame+0x50CAE, then diverges.
            argb = replaceRgb(argb, colors.colors[barsText]);
        } else if (withStatusBar && (argb == platformTextArgb || argb == statusMutedTextArgb) &&
                   caller == graphicsBase + 0x79821 && isStatusBarText(regs)) {
            argb = replaceRgb(argb, colors.colors[argb == statusMutedTextArgb ? statusMutedText : statusText]);
        } else if (withWindowTabs && (argb == 0xFF222222 || argb == 0xFF333333) &&
                   caller == graphicsBase + 0x79821 &&
                   callerAt(regs, 0x578) == frameBase + 0x73480F &&
                   callerAt(regs, 0x748) == frameBase + 0x739629 &&
                   callerAt(regs, 0x9A8) == frameBase + windowTabPaintReturn) {
            argb = replaceRgb(argb, colors.colors[windowTabsText]);
        } else if (withProperties && caller == graphicsBase + 0x79821) {
            argb = propertyLabelColor(regs, argb, colors);
        }
        if (withPopupMenu && caller == graphicsBase + 0x79821)
            argb = popupMenuLabelColor(regs, argb, colors);
        regs.R8 = argb;
        regs.Rax = argb;
    } else if (event.ExceptionAddress == sites[workspaceFill].address) {
        // Only the empty workspace's standard fill. Preserve other platform colors.
        const auto original = static_cast<DWORD>(regs.Rax);
        regs.R8 = (theme & enableWorkspace) && original == workspaceBackgroundBgr ?
            toBgr(colors.colors[workspaceBackground]) : original;
    } else if (event.ExceptionAddress == sites[treeTabFill].address) {
        // The next instruction also saves EAX for the platform's brush.
        const auto original = static_cast<DWORD>(regs.Rax);
        if (withTreeTabs &&
            (original == platformActiveTreeTabBgr || original == platformInactiveTreeTabBgr) &&
            isTreeTabStrip(regs.R15)) {
            regs.Rax = toBgr(colors.colors[original == platformActiveTreeTabBgr ? treeTabsActive : treeTabsInactive]);
        }
        regs.Rbx = static_cast<DWORD>(regs.Rax);
        length = sites[treeTabFill].length;
    } else if (event.ExceptionAddress == sites[treeTabText].address) {
        const auto original = static_cast<DWORD>(regs.Rax);
        regs.R8 = withTreeTabs && original == platformTreeTabTextBgr && isTreeTabStrip(regs.Rdi) ?
            toBgr(colors.colors[treeTabsText]) : original;
    } else if (event.ExceptionAddress == sites[toolbarEdgeStroke].address) {
        const auto original = static_cast<DWORD>(regs.Rdi);
        const bool toolbarEdge = (theme & enableToolbarBorder) != 0 &&
            original == barBackgroundBgr &&
            callerAt(regs, 0x38) == graphicsBase + 0xAF1DB &&
            callerAt(regs, 0xF8) == frameBase + 0x8BD1BF &&
            callerAt(regs, 0x1B8) == frameBase + 0x8BB658;
        regs.Rdx = toolbarEdge ? toBgr(colors.colors[toolbarBorder]) : original;
        length = sites[toolbarEdgeStroke].length;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    regs.Rip = reinterpret_cast<ULONG_PTR>(event.ExceptionAddress) + length;
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
    sites[treeTabFill].address = reinterpret_cast<BYTE*>(frameBase + 0x5D4B1E);
    sites[treeTabText].address = reinterpret_cast<BYTE*>(frameBase + 0x5D4104);
    sites[toolbarEdgeStroke].address = reinterpret_cast<BYTE*>(graphicsBase + 0xAEDA3);
    for (size_t i = 0; i < installedSiteCount; ++i) {
        const auto& site = sites[i];
        if (std::memcmp(site.address, site.instruction.data(), site.length) != 0)
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
    for (size_t i = 0; i < installedSiteCount; ++i) {
        auto& site = sites[i];
        if (!VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &site.protection)) {
            error = GetLastError();
            break;
        }
        ++prepared;
    }
    if (!error) {
        for (size_t i = 0; i < installedSiteCount; ++i) {
            auto& site = sites[i];
            InterlockedExchange8(reinterpret_cast<volatile char*>(site.address),
                                 static_cast<char>(0xCC));
            if (!FlushInstructionCache(GetCurrentProcess(), site.address, 1)) {
                error = GetLastError();
                break;
            }
        }
        if (error) {
            for (size_t i = 0; i < installedSiteCount; ++i) {
                auto& site = sites[i];
                InterlockedExchange8(reinterpret_cast<volatile char*>(site.address),
                                     static_cast<char>(site.instruction.front()));
                FlushInstructionCache(GetCurrentProcess(), site.address, 1);
            }
        }
    }
    // Sites on the same page can observe a previous site's writable protection.
    while (prepared != 0) {
        auto& site = sites[--prepared];
        DWORD ignored{};
        if (!VirtualProtect(site.address, 1, site.protection, &ignored) && !error)
            error = GetLastError();
    }
    // A thread may still be returning from a breakpoint after rollback.
    // Keep the handler and addresses valid for the lifetime of this session.
    return error;
}

DWORD applyProfile(const ThemeProfile& profile, ULONG_PTR zones)
{
    if (zones && (installationResult == ERROR_NOT_READY || installationResult == ERROR_MOD_NOT_FOUND))
        installationResult = installSites();
    if (zones && installationResult != ERROR_SUCCESS) return installationResult;

    const HRESULT captionResult = (zones & enableTreeAndCaption) ? enableCaption(profile) : restoreCaption();
    if (FAILED(captionResult)) return static_cast<DWORD>(captionResult);
    activeProfile.store(&profile, std::memory_order_relaxed);
    activeTheme.store(zones, std::memory_order_release);
    return ERROR_SUCCESS;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(LPVOID setting)
{
    const auto requested = reinterpret_cast<ULONG_PTR>(setting);
    if (requested > 255) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&installationLock);
    const DWORD result = customApplied ? ERROR_ALREADY_ASSIGNED :
        applyProfile(defaultProfile, requested ? requested | enableTreeOnly : 0);
    ReleaseSRWLockExclusive(&installationLock);
    return result;
}

#ifndef THEME_BACKGROUND_ONLY
extern "C" __declspec(dllexport) DWORD WINAPI SetThemeProfile(LPVOID setting)
{
    if (!setting) return ERROR_INVALID_PARAMETER;
    const auto& profile = *static_cast<const ThemeProfile*>(setting);
    if (profile.size != sizeof(ThemeProfile) || profile.version != themeProfileVersion ||
        profile.zones > themeZoneMask)
        return ERROR_INVALID_PARAMETER;
    for (const auto color : profile.colors) {
        if (color > rgbMask) return ERROR_INVALID_PARAMETER;
    }
    AcquireSRWLockExclusive(&installationLock);
    DWORD result = ERROR_ALREADY_ASSIGNED;
    if (!customApplied) {
        customProfile = profile;
        result = applyProfile(customProfile, profile.zones);
        customApplied = result == ERROR_SUCCESS;
    }
    ReleaseSRWLockExclusive(&installationLock);
    return result;
}
#endif
