#include "platform.hpp"
#include "attach_process.hpp"
#include "list_identity.hpp"
#include "devtool_binding.hpp"
#include "panel_headers.hpp"
#include "dock_splitters.hpp"
#include "scroller_corner.hpp"
#include "scroller_png_hook.hpp"
#ifndef THEME_BACKGROUND_ONLY
#include "designer_chrome.hpp"
#include "enterprise_toggle.hpp"
#pragma comment(lib, "gdi32.lib")
#endif
#ifdef THEME_THUMB_SURFACE_PROBE
#include "thumb_surface_capture.hpp"
#endif

#include <atomic>
#include <cstring>
#include <dwmapi.h>
#include <intrin.h>
#include <optional>

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
constexpr ULONG_PTR treeTabVtable = 0xAD36D8;
constexpr ULONG_PTR windowTabPaintReturn = 0x8EFC0A;
constexpr ULONG_PTR windowPanelPaintReturn = 0x8EF5AD;
constexpr ULONG_PTR propertyPaintReturn = 0x4F60BD;
constexpr ULONG_PTR propertyLinkVtable = 0xACCD58;
constexpr ULONG_PTR sharedListPaintReturn = 0x8117C5;
// Windows 11 DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR, also buildable with older SDKs.
constexpr DWORD captionColorAttribute = 35;
constexpr DWORD captionTextColorAttribute = 36;
constexpr COLORREF defaultDwmColor = 0xFFFFFFFF;

struct Site {
    BYTE* address;
    std::array<BYTE, 4> instruction;
    DWORD protection;
    BYTE length{3};
};

enum SiteIndex { rowFill, emptyFill, labelColor, workspaceFill, treeTabFill, treeTabText, toolbarEdgeStroke,
    dockFrameFill, mdiFrameFill, dockTitleFill, panelCaptionText };
Site sites[] = {
    {nullptr, {0x44, 0x8B, 0xC6}, 0}, // mov r8d, esi
    {nullptr, {0x0F, 0x45, 0xD0}, 0}, // cmovne edx, eax
    {nullptr, {0x41, 0x8B, 0xC0}, 0}, // mov eax, r8d
    {nullptr, {0x44, 0x8B, 0xC0}, 0}, // mov r8d, eax
    {nullptr, {0x8B, 0xD8}, 0, 2},  // mov ebx, eax
    {nullptr, {0x44, 0x8B, 0xC0}, 0}, // mov r8d, eax
    {nullptr, {0x8B, 0xD7}, 0, 2},  // mov edx, edi
    {nullptr, {0x48, 0x8B, 0xCE}, 0}, // mov rcx, rsi
    {nullptr, {0x49, 0x8B, 0xCE}, 0}, // mov rcx, r14
    {nullptr, {0x48, 0x8B, 0xCE}, 0}, // mov rcx, rsi
    {nullptr, {0x49, 0x8B, 0xD4}, 0}, // mov rdx, r12
    {nullptr, {0x48, 0x8D, 0x55, 0xC0}, 0, 4}, // lea rdx, [rbp-40h]
    {nullptr, {0x48, 0x8D, 0x55, 0xC0}, 0, 4},
    {nullptr, {0x48, 0x8D, 0x55, 0xC0}, 0, 4},
    {nullptr, {0x48, 0x8D, 0x55, 0xC8}, 0, 4}, // lea rdx, [rbp-38h]
    {nullptr, {0x48, 0x8D, 0x55, 0xC8}, 0, 4},
};
static_assert(std::size(sites) == dockFrameFill + panel_detail::patches.size());
#ifdef THEME_BACKGROUND_ONLY
constexpr size_t installedSiteCount = 2;
#else
constexpr size_t installedSiteCount = std::size(sites);
#endif

ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
ULONG_PTR frontendBase;
ULONG_PTR wbaseBase;
SRWLOCK installationLock = SRWLOCK_INIT;
constexpr ThemeProfile defaultProfile{};
constexpr ThemeProfile disabledProfile{sizeof(ThemeProfile), themeProfileVersion, 0};
// A breakpoint handler may still be reading an earlier profile. Never recycle its slot.
std::array<std::optional<ThemeProfile>, 1024> profileHistory;
size_t profileCount{};
std::atomic<const ThemeProfile*> activeProfile{&disabledProfile};
DWORD installationResult = ERROR_NOT_READY;

void redrawSearchResults() noexcept
{
    const DWORD pid = GetCurrentProcessId();
    const auto target = findDesignerMainWindow(pid);
    if (target.window && !IsIconic(target.window) && designerMainWindow(target.window, pid) == target)
        RedrawWindow(target.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

constexpr devtool_detail::BindingOps bindingOps{
    CreateThreadpoolTimer, SetThreadpoolTimer, GetTickCount64,
    devtool_detail::inspectLoadedModule, redrawSearchResults
};
devtool_detail::Binding devtoolBinding(installationLock, bindingOps);

using CaptionTarget = DesignerWindow;

struct CaptionColors {
    COLORREF background{defaultDwmColor};
    COLORREF text{defaultDwmColor};
};

struct CaptionState {
    CaptionTarget target;
    CaptionColors colors;
};

CaptionState appliedCaption;
bool captionStateUncertain;

bool captionStillOwned(const CaptionTarget& target)
{
    if (!target.window) return false;
    DWORD pid{};
    const DWORD thread = GetWindowThreadProcessId(target.window, &pid);
    return thread == target.thread && pid == GetCurrentProcessId();
}

HRESULT setCaptionColors(const CaptionTarget& target, CaptionColors colors, bool rollback = false)
{
    if (!captionStillOwned(target)) return rollback ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    const HRESULT fill = DwmSetWindowAttribute(target.window, captionColorAttribute,
                                               &colors.background, sizeof(colors.background));
    if (FAILED(fill) && !rollback) return fill;
    if (!captionStillOwned(target)) return rollback ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    const HRESULT text = DwmSetWindowAttribute(target.window, captionTextColorAttribute,
                                               &colors.text, sizeof(colors.text));
    return FAILED(fill) ? fill : text;
}

bool rollbackCaption(const CaptionTarget& target, CaptionColors colors)
{
    return SUCCEEDED(setCaptionColors(target, colors, true)) || !captionStillOwned(target);
}

DWORD captionFailure(HRESULT error, bool rolledBack)
{
    if (rolledBack) return static_cast<DWORD>(error);
    captionStateUncertain = true;
    return themeStateUncertain;
}

DWORD transitionCaption(const CaptionTarget& next, CaptionColors colors)
{
    if (captionStateUncertain) return themeStateUncertain;
    CaptionState previous = appliedCaption;
    if (!captionStillOwned(previous.target)) previous = {};
    const bool moved = previous.target != next;
    if (previous.target.window && moved) {
        const auto reset = setCaptionColors(previous.target, {});
        if (FAILED(reset))
            return captionFailure(reset, rollbackCaption(previous.target, previous.colors));
    }
    if (next.window) {
        const auto set = setCaptionColors(next, colors);
        if (FAILED(set)) {
            // DWM exposes setters for these attributes. New stock windows start at defaults.
            const bool resetNew = rollbackCaption(next, moved ? CaptionColors{} : previous.colors);
            const bool resetOld = !previous.target.window || !moved ||
                rollbackCaption(previous.target, previous.colors);
            return captionFailure(set, resetNew && resetOld);
        }
    }
    appliedCaption = {next, colors};
    return ERROR_SUCCESS;
}

DWORD applyCaption(const ThemeProfile& profile, ULONG_PTR zones, const DesignerWindow& mainWindow)
{
    if (!(zones & enableTreeAndCaption)) return transitionCaption({}, {});
    return transitionCaption(mainWindow, {toBgr(profile.colors[captionBackground]),
                                          toBgr(profile.colors[captionText])});
}

DWORD replaceRgb(DWORD value, DWORD rgb)
{
    return (value & ~rgbMask) | rgb;
}

ULONG_PTR callerAt(const CONTEXT& registers, ULONG_PTR offset)
{
    const auto* stack = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto bottom = reinterpret_cast<ULONG_PTR>(stack->StackLimit);
    if (registers.Rsp < bottom || offset > (std::numeric_limits<ULONG_PTR>::max)() - registers.Rsp)
        return 0;
    const auto location = registers.Rsp + offset;
    const auto top = reinterpret_cast<ULONG_PTR>(stack->StackBase);
    if (top < sizeof(ULONG_PTR) || location > top - sizeof(ULONG_PTR) || location % alignof(ULONG_PTR))
        return 0;
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(location);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

ListKind listKindAt(const CONTEXT& registers, ULONG_PTR paintReturnSlot)
{
    if (callerAt(registers, paintReturnSlot) != frameBase + sharedListPaintReturn)
        return ListKind::Unknown;

    // The paint prologue saves the dispatcher's RDI (receiver + 0x100).
    const auto savedRdi = callerAt(registers, paintReturnSlot - 0x20);
    return savedRdi > 0x100 ?
        classifyListControl(savedRdi - 0x100, frameBase, frontendBase, devtoolBinding.base()) :
        ListKind::Unknown;
}

DWORD listColor(ListKind kind, DWORD original, ULONG_PTR zones,
                const ThemeProfile& palette, bool text, bool useBgr)
{
    ULONG_PTR zone{};
    PaletteColor color{};
    switch (kind) {
    case ListKind::MetadataTree:
        zone = enableTreeOnly; color = text ? treeText : treeBackground; break;
    case ListKind::Dropdown:
        // Built-in presets leave the optional dropdown override disabled.
        // Follow the properties palette unless a separate list palette was requested.
        if (zones & enableDropdown) {
            zone = enableDropdown; color = text ? dropdownText : dropdownBackground;
        } else {
            zone = enableTreeAndProperties; color = text ? propertyText : propertyBackground;
        }
        break;
    case ListKind::SearchResults:
        zone = enableSearchResults; color = text ? searchResultsText : searchResultsBackground; break;
    default: return original;
    }
    if (!(zones & zone)) return original;
    const DWORD rgb = palette.colors[color];
    return replaceRgb(original, useBgr ? toBgr(rgb) : rgb);
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
        (caller == frameBase + 0x4F9A75 || caller == frameBase + 0x4FA167);
    const bool selected = bgr == 0x00C26A53 && caller == frameBase + 0x4F9C21;
    if ((background || selected) &&
        callerAt(registers, 0x538) == frameBase + propertyPaintReturn)
        return toBgr(profile.colors[selected ? propertySelection : propertyBackground]);
    return bgr;
}

bool isPropertyLink(const CONTEXT& registers)
{
    // The graphics entry point saves the label receiver in its RDI slot.
    const auto receiver = callerAt(registers, 0x560);
    if (!receiver) return false;
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(receiver) == frameBase + propertyLinkVtable;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

DWORD propertyLabelColor(const CONTEXT& registers, DWORD argb, const ThemeProfile& profile)
{
    // Static dialog labels share the renderer with links in the property palette.
    if ((argb == platformTextArgb || argb == platformMutedTextArgb) &&
        callerAt(registers, 0x578) == frameBase + 0x5BD040 &&
        callerAt(registers, 0x608) == frameBase + 0x5B9363 &&
        callerAt(registers, 0xA78) == frameBase + 0x5B5C2D &&
        isPropertyLink(registers))
        return replaceRgb(argb, profile.colors[argb == platformTextArgb ? propertyText : propertyMutedText]);

    // The search renderer draws the matching and ordinary fragments separately.
    if (argb == platformTextArgb &&
        callerAt(registers, 0x688) == frameBase + 0x4F9FC9 &&
        callerAt(registers, 0x9A8) == frameBase + propertyPaintReturn)
        return replaceRgb(argb, profile.colors[propertyText]);

    const auto caller = callerAt(registers, 0x4F8);
    if ((caller != frameBase + 0x4F9DD4 && caller != frameBase + 0x4FA3DD) ||
        callerAt(registers, 0x818) != frameBase + propertyPaintReturn) return argb;

    if (caller == frameBase + 0x4FA3DD && argb == 0xFF594304)
        return replaceRgb(argb, profile.colors[propertyGroupText]);
    if (caller == frameBase + 0x4F9DD4) {
        if (argb == platformTextArgb) return replaceRgb(argb, profile.colors[propertyText]);
        if (argb == platformMutedTextArgb) return replaceRgb(argb, profile.colors[propertyMutedText]);
    }
    return argb;
}

bool isStatusBarText(const CONTEXT& registers)
{
    if (callerAt(registers, 0x578) != frameBase + 0x7347DF) return false;

    // The message uses a different text renderer with a larger stack frame.
    const auto renderer = callerAt(registers, 0x748);
    const bool message = renderer == frameBase + 0x73B4A7;
    if (!message && renderer != frameBase + 0x739469 && renderer != frameBase + 0x739778)
        return false;

    const ULONG_PTR shift = message ? 0x10 : 0;
    return callerAt(registers, 0x8A8 + shift) == frameBase + 0x72A336 &&
           callerAt(registers, 0x9F8 + shift) == frameBase + 0x70FB75 &&
           callerAt(registers, 0xBB8 + shift) == frameBase + 0x73210F;
}

DWORD popupMenuFillColor(const CONTEXT& registers, DWORD bgr, const ThemeProfile& palette)
{
    if (callerAt(registers, 0x278) != frameBase + 0x70FB4B ||
        callerAt(registers, 0x328) != frameBase + 0x50CAE ||
        callerAt(registers, 0x438) != frameBase + popupMenuPaintReturn)
        return bgr;

    const auto painter = callerAt(registers, 0x218);
    if (bgr == platformPopupMenuBackgroundBgr && painter == frameBase + 0x7251D5)
        return toBgr(palette.colors[popupMenuBackground]);
    if (bgr == platformPopupMenuGutterBgr && painter == frameBase + 0x725285)
        return toBgr(palette.colors[popupMenuGutter]);
    return bgr;
}

DWORD popupMenuLabelColor(const CONTEXT& registers, DWORD argb, const ThemeProfile& palette)
{
    const bool shortcut = argb == platformPopupShortcutArgb;
    if ((!shortcut && argb != platformTextArgb) ||
        callerAt(registers, 0xB68) != frameBase + popupMenuPaintReturn ||
        callerAt(registers, 0x8A8) != frameBase + 0x70FC6C ||
        callerAt(registers, 0x748) != frameBase + 0x739469 ||
        callerAt(registers, 0x578) != frameBase + (shortcut ? 0x73496F : 0x7347DF))
        return argb;
    return replaceRgb(argb, palette.colors[shortcut ? popupMenuShortcut : popupMenuText]);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto& event = *exception->ExceptionRecord;
    if (event.ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;

    const auto lastError = GetLastError();
    auto& regs = *exception->ContextRecord;
    const auto& colors = *activeProfile.load(std::memory_order_acquire);
    const ULONG_PTR theme = colors.zones;
    const bool dark = (theme & enableTreeOnly) != 0;
    const bool withLists = (theme & (enableTreeOnly | enableDropdown | enableSearchResults |
                                     enableTreeAndProperties)) != 0;
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
        regs.R8 = withLists && (original & rgbMask) == white ?
            listColor(listKindAt(regs, 0x3D8), original, theme, colors, false, true) : original;
    } else if (event.ExceptionAddress == sites[emptyFill].address) {
        // CMOVNE selects the low 32 bits and clears the high 32 bits.
        regs.Rdx = static_cast<DWORD>((regs.EFlags & 0x40) == 0 ? regs.Rax : regs.Rdx);
        bool paletteFillMatched = false;
        if (withLists && static_cast<DWORD>(regs.Rdx) == white &&
            callerAt(regs, 0x98) == graphicsBase + 0xABE0C &&
            callerAt(regs, 0x148) == frameBase + 0xA4638) {
            // A direct grid fill and a tree fill have different enclosing frames.
            const auto kind = callerAt(regs, 0x198) == frameBase + 0x8119D8 ?
                classifyListControl(callerAt(regs, 0x190), frameBase, frontendBase, devtoolBinding.base()) :
                listKindAt(regs, 0x268);
            regs.Rdx = listColor(kind, white, theme, colors, false, true);
        } else if ((theme & enableDockSplitter) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   dock_detail::matches(regs, frameBase, graphicsBase)) {
            regs.Rdx = toBgr(colors.colors[dockSplitterFill]);
            paletteFillMatched = true;
        } else if ((theme & enableScrollerCorner) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == scroller_corner::platformFillBgr &&
                   scroller_corner::matches(regs, frameBase, graphicsBase, wbaseBase, scroller_corner::Paint::Fill)) {
            regs.Rdx = toBgr(colors.colors[scrollerCornerBackground]);
            paletteFillMatched = true;
        } else if ((theme & enableToolbarBand) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x98) == graphicsBase + 0xABE0C &&
                   callerAt(regs, 0x148) == frameBase + 0x8E9CF7) {
            regs.Rdx = toBgr(colors.colors[toolbarBandBackground]);
        } else if ((theme & enableToolbarBorder) != 0 && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x98) == graphicsBase + 0xB80EF &&
                   callerAt(regs, 0x1E8) == frameBase + 0x8BD1AD &&
                   callerAt(regs, 0x2A8) == frameBase + 0x8BB628) {
            regs.Rdx = toBgr(colors.colors[toolbarBorder]);
        } else if ((withBars || withStatusBar || withWindowTabs) && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == barBackgroundBgr &&
                   callerAt(regs, 0x218) == frameBase + 0x71A6AF) {
            const bool toolbar = withBars && callerAt(regs, 0x2E8) == graphicsBase + 0xAD215;
            const bool status = withStatusBar &&
                callerAt(regs, 0x2B8) == frameBase + 0x70FB4B &&
                callerAt(regs, 0x478) == frameBase + 0x73210F;
            const bool windowPanel = withWindowTabs &&
                callerAt(regs, 0x2B8) == frameBase + 0x70FB4B &&
                callerAt(regs, 0x478) == frameBase + windowPanelPaintReturn;
            if (toolbar) regs.Rdx = toBgr(colors.colors[barsBackground]);
            if (status) regs.Rdx = toBgr(colors.colors[statusBackground]);
            if (windowPanel) regs.Rdx = toBgr(colors.colors[windowTabsBackground]);
        } else if (withWindowTabs && (regs.EFlags & 0x40) == 0 &&
                   (regs.Rdx == platformActiveWindowTabBgr ||
                    regs.Rdx == platformInactiveWindowTabBgr) &&
                   callerAt(regs, 0x158) == frameBase + 0x733F9F &&
                   callerAt(regs, 0x328) == frameBase + 0x7395F9 &&
                   callerAt(regs, 0x588) == frameBase + windowTabPaintReturn) {
            regs.Rdx = regs.Rdx == platformActiveWindowTabBgr ?
                toBgr(colors.colors[windowTabsActive]) : toBgr(colors.colors[windowTabsInactive]);
        } else if (withTreeTabs && (regs.EFlags & 0x40) == 0 &&
                   regs.Rdx == platformActiveTreeTabBgr &&
                   callerAt(regs, 0x148) == frameBase + 0x5D33A5 &&
                   callerAt(regs, 0x1F8) == frameBase + 0x5CE319) {
            regs.Rdx = toBgr(colors.colors[treeTabsBackground]);
        } else if (withProperties && (regs.EFlags & 0x40) == 0) {
            regs.Rdx = propertyFillColor(regs, static_cast<DWORD>(regs.Rdx), colors);
        }
        if (!paletteFillMatched && withPopupMenu && (regs.EFlags & 0x40) == 0 &&
            (regs.Rdx == platformPopupMenuBackgroundBgr || regs.Rdx == platformPopupMenuGutterBgr))
            regs.Rdx = popupMenuFillColor(regs, static_cast<DWORD>(regs.Rdx), colors);
    } else if (event.ExceptionAddress == sites[labelColor].address) {
        auto argb = static_cast<DWORD>(regs.R8);
        const auto caller = callerAt(regs, 0x38);
        if (withLists && (argb & rgbMask) == white && caller == graphicsBase + 0x7878B &&
            callerAt(regs, 0x4B8) == frameBase + 0x85F445) {
            argb = listColor(listKindAt(regs, 0xCB8), argb, theme, colors, false, false);
        } else if (withLists && (argb & rgbMask) == 0 && caller == graphicsBase + 0x78F51) {
            if (callerAt(regs, 0x578) == frameBase + 0x85F445) {
                argb = listColor(listKindAt(regs, 0xD78), argb, theme, colors, true, false);
            } else if (dark && callerAt(regs, 0x688) == frameBase + 0x85F29D &&
                       listKindAt(regs, 0xE88) == ListKind::MetadataTree) {
                // Separate fragments of filtered tree labels.
                argb = replaceRgb(argb, colors.colors[treeText]);
            }
        } else if (withBars && argb == platformTextArgb && caller == graphicsBase + 0x78F51 &&
                   callerAt(regs, 0x578) == frameBase + 0x7347DF &&
                   callerAt(regs, 0x748) == frameBase + 0x739469 &&
                   callerAt(regs, 0x8A8) == frameBase + 0x70FC6C &&
                   callerAt(regs, 0xB68) == frameBase + 0x5091F) {
            // The popup shares the renderer through frame+0x50CAE, then diverges.
            argb = replaceRgb(argb, colors.colors[barsText]);
        } else if (withStatusBar && (argb == platformTextArgb || argb == statusMutedTextArgb) &&
                   caller == graphicsBase + 0x78F51 && isStatusBarText(regs)) {
            argb = replaceRgb(argb, colors.colors[argb == statusMutedTextArgb ? statusMutedText : statusText]);
        } else if (withWindowTabs && (argb == 0xFF222222 || argb == 0xFF333333) &&
                   caller == graphicsBase + 0x78F51 &&
                   callerAt(regs, 0x578) == frameBase + 0x7347DF &&
                   callerAt(regs, 0x748) == frameBase + 0x7395F9 &&
                   callerAt(regs, 0x9A8) == frameBase + windowTabPaintReturn) {
            argb = replaceRgb(argb, colors.colors[windowTabsText]);
        } else if (withProperties && caller == graphicsBase + 0x78F51) {
            argb = propertyLabelColor(regs, argb, colors);
        }
        if (withPopupMenu && caller == graphicsBase + 0x78F51)
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
            callerAt(regs, 0x38) == graphicsBase + 0xAE90B &&
            callerAt(regs, 0xF8) == frameBase + 0x8BD18F &&
            callerAt(regs, 0x1B8) == frameBase + 0x8BB628;
        regs.Rdx = toolbarEdge ? toBgr(colors.colors[toolbarBorder]) : original;
        if ((theme & enableScrollerCorner) != 0 && original == scroller_corner::platformStrokeBgr &&
            scroller_corner::matches(regs, frameBase, graphicsBase, wbaseBase, scroller_corner::Paint::Stroke))
            regs.Rdx = toBgr(colors.colors[scrollerCornerBorder]);
        length = sites[toolbarEdgeStroke].length;
    } else {
        size_t site = dockFrameFill;
        while (site < installedSiteCount && event.ExceptionAddress != sites[site].address) ++site;
        if (site >= installedSiteCount) return EXCEPTION_CONTINUE_SEARCH;
        panel_detail::apply(regs, frameBase, static_cast<panel_detail::Paint>(site - dockFrameFill), colors);
        length = sites[site].length;
    }
    regs.Rip = reinterpret_cast<ULONG_PTR>(event.ExceptionAddress) + length;
    SetLastError(lastError);
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD patchSites();

bool panelPatchSignaturesMatch()
{
    for (size_t i = 0; i < panel_detail::patches.size(); ++i) {
        const auto& patch = panel_detail::patches[i];
        if (std::memcmp(sites[dockFrameFill + i].address, patch.bytes.data(), patch.bytes.size()) != 0)
            return false;
    }
    return true;
}

DWORD installSites()
{
    const auto frame = GetModuleHandleW(L"frame.dll");
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto frontend = GetModuleHandleW(L"frntend.dll");
    const auto wbase = GetModuleHandleW(L"wbase83.dll");
    if (!frame || !graphics || !frontend || !wbase) return ERROR_MOD_NOT_FOUND;
    if (IsDebuggerPresent() || !matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash) ||
        !matchesSha256(modulePath(graphics), graphicsHash) ||
        !matchesSha256(modulePath(frontend), listFrontendHash) ||
        !matchesSha256(modulePath(wbase), wbaseHash)) return ERROR_REVISION_MISMATCH;

    frameBase = reinterpret_cast<ULONG_PTR>(frame);
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    frontendBase = reinterpret_cast<ULONG_PTR>(frontend);
    wbaseBase = reinterpret_cast<ULONG_PTR>(wbase);
    sites[rowFill].address = reinterpret_cast<BYTE*>(frameBase + 0x303424);
    sites[emptyFill].address = reinterpret_cast<BYTE*>(graphicsBase + 0xAE57E);
    sites[labelColor].address = reinterpret_cast<BYTE*>(graphicsBase + 0x22D7);
    sites[workspaceFill].address = reinterpret_cast<BYTE*>(frameBase + 0x9518BC);
    sites[treeTabFill].address = reinterpret_cast<BYTE*>(frameBase + 0x5D4AEE);
    sites[treeTabText].address = reinterpret_cast<BYTE*>(frameBase + 0x5D40D4);
    sites[toolbarEdgeStroke].address = reinterpret_cast<BYTE*>(graphicsBase + 0xAE4D3);
    for (size_t i = 0; i < panel_detail::patches.size(); ++i)
        sites[dockFrameFill + i].address = reinterpret_cast<BYTE*>(frameBase + panel_detail::patches[i].rva);
    for (size_t i = 0; i < installedSiteCount; ++i) {
        const auto& site = sites[i];
        if (std::memcmp(site.address, site.instruction.data(), site.length) != 0)
            return ERROR_INVALID_DATA;
    }
    if (installedSiteCount == std::size(sites) && !panelPatchSignaturesMatch()) return ERROR_INVALID_DATA;

    // The process owns the handler and these module addresses until exit.
    const LPCWSTR modules[] = {
        reinterpret_cast<LPCWSTR>(&onBreakpoint),
        reinterpret_cast<LPCWSTR>(frame),
        reinterpret_cast<LPCWSTR>(graphics),
        reinterpret_cast<LPCWSTR>(frontend),
        reinterpret_cast<LPCWSTR>(wbase),
    };
    constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    for (const auto& module : modules) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(flags, module, &pinned)) return GetLastError();
    }

    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    if (!handler) return ERROR_INVALID_FUNCTION;
    return patchSites();
}

DWORD patchSites()
{
    size_t prepared = 0;
    DWORD error = ERROR_SUCCESS;
    bool rollbackUncertain = false;
    for (size_t i = 0; i < installedSiteCount; ++i) {
        auto& site = sites[i];
        if (!VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &site.protection)) {
            error = GetLastError();
            break;
        }
        ++prepared;
    }
    if (!error) {
        size_t patched = 0;
        for (size_t i = 0; i < installedSiteCount; ++i) {
            auto& site = sites[i];
            const auto original = static_cast<char>(site.instruction.front());
            if (_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(site.address),
                                             static_cast<char>(0xCC), original) != original) {
                error = ERROR_INVALID_DATA;
                break;
            }
            ++patched;
            if (!FlushInstructionCache(GetCurrentProcess(), site.address, 1)) {
                error = GetLastError();
                break;
            }
        }
        if (error) {
            while (patched) {
                auto& site = sites[--patched];
                constexpr auto breakpoint = static_cast<char>(0xCC);
                if (_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(site.address),
                                                static_cast<char>(site.instruction.front()), breakpoint) != breakpoint ||
                    !FlushInstructionCache(GetCurrentProcess(), site.address, 1))
                    rollbackUncertain = true;
            }
        }
    }
    // Sites on the same page can observe a previous site's writable protection.
    while (prepared != 0) {
        auto& site = sites[--prepared];
        DWORD ignored{};
        if (!VirtualProtect(site.address, 1, site.protection, &ignored))
            rollbackUncertain = true;
    }
    // A thread may still be returning from a breakpoint after rollback.
    // Keep the handler and addresses valid for the lifetime of this session.
    return rollbackUncertain ? themeStateUncertain : error;
}

DWORD applyProfile(const ThemeProfile& profile, ULONG_PTR zones)
{
    if (installationResult == themeStateUncertain || captionStateUncertain) return themeStateUncertain;
    const DWORD pid = GetCurrentProcessId();
    const auto mainWindow = zones ? findDesignerMainWindow(pid) : DesignerWindow{};
    if (zones && !mainWindow.window) return ERROR_NOT_READY;
    const ThemeProfile* snapshot = &disabledProfile;
    size_t index{};
    if (zones) {
        while (index < profileCount && (profileHistory[index]->zones != zones ||
                                        profileHistory[index]->colors != profile.colors)) ++index;
        if (index == profileHistory.size()) return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (zones && (installationResult == ERROR_NOT_READY || installationResult == ERROR_MOD_NOT_FOUND))
        installationResult = installSites();
    if (zones && installationResult != ERROR_SUCCESS) return installationResult;
#ifndef THEME_BACKGROUND_ONLY
    if (zones & (enableTreeAndBars | enableTreeAndProperties | enableTreeTabs | enableScrollerPng |
                 enableDockSplitter)) {
        const auto chromeResult=designer_chrome::ensure(frameBase,graphicsBase,activeProfile);
        if (chromeResult) return chromeResult;
    }
#endif
    if (zones & (enableScrollerPng | enableScrollerThumb)) {
        const auto pngResult = scroller_png_hook::ensure(frameBase, graphicsBase, wbaseBase, activeProfile);
        if (pngResult) return pngResult;
    }
    if (zones & enableScrollerThumb) {
        const auto thumbResult = scroller_png_hook::ensureThumb();
        if (thumbResult) return thumbResult;
    }

    if (zones && designerMainWindow(mainWindow.window, pid) != mainWindow) return ERROR_NOT_READY;
    if (const auto captionResult = applyCaption(profile, zones, mainWindow)) {
        if (captionResult == static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)))
            return ERROR_NOT_READY;
        return captionResult;
    }
    if (zones) {
        if (index == profileCount) {
            auto& entry = profileHistory[profileCount++].emplace(profile);
            entry.zones = static_cast<DWORD>(zones);
        }
        snapshot = &*profileHistory[index];
    }
    activeProfile.store(snapshot, std::memory_order_release);
    devtoolBinding.setEnabledLocked((zones & enableSearchResults) != 0, reinterpret_cast<HMODULE>(frameBase));
    return ERROR_SUCCESS;
}

bool inDesigner()
{
    static const bool designer = isDesignerCommandLine(GetCommandLineW());
    return designer;
}

#ifndef THEME_BACKGROUND_ONLY
std::atomic<const ThemeProfile*> rememberedDesignerProfile{&defaultProfile};

bool designerThemeEnabled()
{
    return activeProfile.load(std::memory_order_acquire)->zones != 0;
}

HWND designerControlWindow()
{
    return findDesignerMainWindow(GetCurrentProcessId()).window;
}

ThemeProfile designerControlProfile()
{
    return *rememberedDesignerProfile.load(std::memory_order_acquire);
}

DWORD applyDesignerProfileLocked(const ThemeProfile& profile)
{
    if (profile.size != sizeof(ThemeProfile) || profile.version != themeProfileVersion ||
        profile.zones > themeZoneMask) return ERROR_INVALID_PARAMETER;
    for (const auto color : profile.colors) {
        if (color > rgbMask) return ERROR_INVALID_PARAMETER;
    }
    const auto result=applyProfile(profile,profile.zones);
    if (!result && profile.zones)
        rememberedDesignerProfile.store(activeProfile.load(std::memory_order_acquire),std::memory_order_release);
    return result;
}

bool applyDesignerControlProfile(const ThemeProfile& profile)
{
    AcquireSRWLockExclusive(&installationLock);
    const auto result=applyDesignerProfileLocked(profile);
    ReleaseSRWLockExclusive(&installationLock);
    return result==ERROR_SUCCESS;
}

void toggleDesignerTheme()
{
    AcquireSRWLockExclusive(&installationLock);
    applyDesignerProfileLocked(designerThemeEnabled() ? disabledProfile : designerControlProfile());
    ReleaseSRWLockExclusive(&installationLock);
}

bool copyThemeProfile(LPVOID setting, ThemeProfile& profile)
{
    if (!setting) return false;
    __try {
        const auto& supplied = *static_cast<const ThemeProfile*>(setting);
        if (supplied.size != sizeof(ThemeProfile) || supplied.version != themeProfileVersion)
            return false;
        profile = supplied;
        return profile.size == sizeof(ThemeProfile) && profile.version == themeProfileVersion;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
#endif

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(LPVOID setting)
{
    const auto requested = reinterpret_cast<ULONG_PTR>(setting);
    if (requested & ~(static_cast<ULONG_PTR>(255) | enableSearchResults))
        return ERROR_INVALID_PARAMETER;
    if (!inDesigner()) return ERROR_NOT_SUPPORTED;
    AcquireSRWLockExclusive(&installationLock);
    const DWORD result = applyProfile(defaultProfile, requested ? requested | enableTreeOnly : 0);
    ReleaseSRWLockExclusive(&installationLock);
    return result;
}

#ifdef THEME_THUMB_SURFACE_PROBE
extern "C" __declspec(dllexport) DWORD WINAPI ThumbSurfaceProbeAbiV5(LPVOID)
{
    return thumbSurfaceProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunThumbSurfaceProbeV5(LPVOID output)
{
    if (!inDesigner()) return ERROR_NOT_SUPPORTED;
    return thumb_surface_capture::run(output);
}
#endif

#ifndef THEME_BACKGROUND_ONLY
extern "C" __declspec(dllexport) DWORD WINAPI ThemeProfileAttachV11(LPVOID)
{
    return themeProfileVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI SetThemeProfile(LPVOID setting)
{
    if (!inDesigner()) return ERROR_NOT_SUPPORTED;
    ThemeProfile profile;
    if (!copyThemeProfile(setting,profile)) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&installationLock);
    const DWORD result=applyDesignerProfileLocked(profile);
    ReleaseSRWLockExclusive(&installationLock);
    if (!result && profile.zones)
        enterprise_toggle::start(designerThemeEnabled,toggleDesignerTheme,nullptr,nullptr,nullptr,
                                 applyDesignerControlProfile,designerControlProfile,designerControlWindow);
    return result;
}
#endif
