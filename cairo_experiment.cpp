#include "platform.hpp"
#ifdef THEME_TOOLTIP_TRACE
#include "tooltip_trace.hpp"
#elif defined(THEME_FORM_LINE_TRACE)
#include "form_line_trace.hpp"
#include <cstring>
#elif defined(THEME_OUTLINE_TRACE)
#include "outline_trace.hpp"
#include <cstring>
#elif defined(THEME_GRID_TRACE)
#include "grid_trace.hpp"
#include <cmath>
#endif

#if defined(THEME_OUTLINE_TRACE) || defined(THEME_GRID_TRACE)
#define THEME_SPREADSHEET_TRACE
#endif
#if defined(THEME_TOOLTIP_TRACE) || defined(THEME_FORM_LINE_TRACE) || defined(THEME_SPREADSHEET_TRACE)
#define THEME_CAIRO_TRACE
#endif

#include <intrin.h>
#include <atomic>

namespace {

using SetSourceRgb = void (*)(void*, double, double, double);
using SetSourceRgba = void (*)(void*, double, double, double, double);
constexpr ULONG_PTR sourceRgbSlot = 0xC7B60;
constexpr ULONG_PTR sourceRgbaSlot = 0xC7C20;
#ifndef THEME_SPREADSHEET_TRACE
constexpr ULONG_PTR solidFillReturn = 0xAEE56;
constexpr ULONG_PTR treeRowsReturn = 0x303466;
constexpr ULONG_PTR listPaintReturn = 0x8117F5;
constexpr ULONG_PTR treePaintVtable = 0xA88AC0;
#endif

struct ImportHook {
    ULONG_PTR offset;
    const char* name;
    PVOID replacement;
    PVOID original{};
    PVOID volatile* slot{};
};

void setSourceRgb(void*, double, double, double);
void setSourceRgba(void*, double, double, double, double);
#ifdef THEME_GRID_TRACE
void cairoRectangle(void*, double, double, double, double);
#endif

ImportHook hooks[] = {
    {sourceRgbSlot, "cairo_set_source_rgb", reinterpret_cast<PVOID>(&setSourceRgb)},
    {sourceRgbaSlot, "cairo_set_source_rgba", reinterpret_cast<PVOID>(&setSourceRgba)},
#ifdef THEME_GRID_TRACE
    {0, "cairo_rectangle", reinterpret_cast<PVOID>(&cairoRectangle)},
#endif
};
#if defined(THEME_TOOLTIP_TRACE) || defined(THEME_FORM_LINE_TRACE)
constexpr size_t firstHook = 1;
#else
constexpr size_t firstHook = 0;
#endif
#ifdef THEME_BACKGROUND_ONLY
constexpr size_t hookCount = 1;
#else
constexpr size_t hookCount = std::size(hooks);
#endif
#ifdef THEME_CAIRO_TRACE
SRWLOCK traceLock = SRWLOCK_INIT;
#ifdef THEME_TOOLTIP_TRACE
TooltipTrace trace;
const TooltipTracePlatform* tracePlatform;
#elif defined(THEME_FORM_LINE_TRACE)
FormLineTrace trace;
const FormLineTracePlatform* tracePlatform;
#elif defined(THEME_GRID_TRACE)
GridTrace trace;
const OutlineTracePlatform* tracePlatform;
const GridTracePlatform* gridPlatform;
#else
OutlineTrace trace;
const OutlineTracePlatform* tracePlatform;
#endif
#endif
ULONG_PTR frameBase;
ULONG_PTR graphicsBase;
SRWLOCK controlLock = SRWLOCK_INIT;
std::atomic<bool> enabled;

#ifdef THEME_SPREADSHEET_TRACE
std::array<std::array<std::atomic<ULONGLONG>, static_cast<size_t>(TraceOutcome::Count)>, trace.stages.size()>
    stageCounts{};

void countOutcome(size_t hook, TraceOutcome outcome)
{
    stageCounts[hook][static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}
#endif

#ifndef THEME_SPREADSHEET_TRACE
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
#endif

#ifdef THEME_CAIRO_TRACE
bool packRgba(double red, double green, double blue, double alpha, DWORD& rgba)
{
    const auto isChannel = [](double value) { return value >= 0 && value <= 1; };
    if (!isChannel(red) || !isChannel(green) || !isChannel(blue) || !isChannel(alpha)) return false;
    const auto byte = [](double value) { return static_cast<DWORD>(value * 255.0 + 0.5); };
    rgba = (byte(red) << 24) | (byte(green) << 16) | (byte(blue) << 8) | byte(alpha);
    return true;
}
#endif

#ifdef THEME_TOOLTIP_TRACE
bool isExpandedTooltip(const ULONG_PTR* stack)
{
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    const auto& renderer = tracePlatform->rendererReturn;
    if (address > stackTop || stackTop - address < renderer.stackOffset + sizeof(*stack)) return false;
    // Walk the verified fixed stack frames back to frame.dll's tooltip renderer.
    for (const auto& entry : tracePlatform->graphics.returns) {
        const auto actual = stack[entry.stackOffset / sizeof(*stack)];
        if (actual != graphicsBase + entry.rva &&
            (!entry.alternateRva || actual != graphicsBase + entry.alternateRva)) return false;
    }
    return stack[renderer.stackOffset / sizeof(*stack)] == frameBase + renderer.rva;
}

void countTooltipColor(double red, double green, double blue, double alpha)
{
    DWORD rgba{};
    if (!packRgba(red, green, blue, alpha, rgba)) return;
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
#elif defined(THEME_FORM_LINE_TRACE)
void recordFormLine(const ULONG_PTR* stack, double red, double green, double blue, double alpha)
{
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    if (address > stackTop || stackTop - address < 0x138 ||
        stack[0x40 / sizeof(*stack)] != graphicsBase + tracePlatform->penReturn ||
        stack[0xE0 / sizeof(*stack)] != graphicsBase + tracePlatform->segmentReturn ||
        stack[0x130 / sizeof(*stack)] != frameBase + tracePlatform->rendererReturn) return;

    DWORD rgba{};
    if (!packRgba(red, green, blue, alpha, rgba)) return;
    // The +0xD0 wrapper keeps both logical points in its stack frame.
    POINT first{}, second{};
    std::memcpy(&first, stack + 0x120 / sizeof(*stack), sizeof(first));
    std::memcpy(&second, stack + 0x118 / sizeof(*stack), sizeof(second));

    AcquireSRWLockExclusive(&traceLock);
    DWORD* count = &trace.otherCalls;
    for (auto& line : trace.lines) {
        if (line.calls && (line.rgba != rgba || line.x1 != first.x || line.y1 != first.y ||
                           line.x2 != second.x || line.y2 != second.y)) continue;
        if (!line.calls) line = {rgba, first.x, first.y, second.x, second.y, 0};
        count = &line.calls;
        break;
    }
    if (*count != MAXDWORD) ++*count;
    ReleaseSRWLockExclusive(&traceLock);
}
#elif defined(THEME_OUTLINE_TRACE)
TraceOutcome recordOutline(const ULONG_PTR* stack, bool mask, double red, double green,
                           double blue, double alpha)
{
    const auto& profile = *tracePlatform;
    const auto expected = mask ? profile.rgbaReturn : profile.graphicsReturns.front();
    if (stack[0] != graphicsBase + expected) return TraceOutcome::Caller;

    constexpr size_t inputColorOffset = 0xF8;
    const size_t converterFrame = mask ? 0x40 : 0;
    const size_t rendererOffset = profile.rendererStackOffset + converterFrame;
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    if (address > stackTop || stackTop - address < rendererOffset + inputColorOffset + sizeof(DWORD))
        return TraceOutcome::Bounds;

    constexpr size_t graphicsOffsets[] = {0, 0x40, 0x90, 0x140};
    for (size_t i = 0; i < std::size(graphicsOffsets); ++i) {
        const auto offset = (converterFrame + graphicsOffsets[i]) / sizeof(*stack);
        if (stack[offset] != graphicsBase + profile.graphicsReturns[i]) return TraceOutcome::Graphics;
    }
    const auto rendererReturn = stack[rendererOffset / sizeof(*stack)] - frameBase;
    if (rendererReturn != profile.rendererReturns[0] && rendererReturn != profile.rendererReturns[1])
        return TraceOutcome::Renderer;

    OutlineTraceSample sample{};
    if (!packRgba(red, green, blue, alpha, sample.rgba)) return TraceOutcome::InvalidSample;
    sample.mask = mask;
    sample.rendererReturn = static_cast<DWORD>(rendererReturn);
    const auto* rendererStack = reinterpret_cast<const BYTE*>(stack) + rendererOffset;
    std::memcpy(&sample.rect, rendererStack + 0x58, sizeof(sample.rect));
    std::memcpy(&sample.inputColor, rendererStack + inputColorOffset, sizeof(sample.inputColor));

    AcquireSRWLockExclusive(&traceLock);
    DWORD* count = &trace.otherCalls;
    for (auto& outline : trace.outlines) {
        if (outline.calls && (outline.mask != sample.mask || outline.rgba != sample.rgba ||
            outline.rendererReturn != sample.rendererReturn || outline.inputColor != sample.inputColor ||
            !EqualRect(&outline.rect, &sample.rect))) continue;
        if (!outline.calls) outline = sample;
        count = &outline.calls;
        break;
    }
    if (*count != MAXDWORD) ++*count;
    ReleaseSRWLockExclusive(&traceLock);
    return TraceOutcome::Match;
}
#elif defined(THEME_GRID_TRACE)
enum class GridEvent { Rgb, Mask, Rectangle };

struct CodeRange {
    ULONG_PTR imageBase{};
    DWORD start{};
    DWORD size{};

    DWORD rva(ULONG_PTR address) const
    {
        const auto offset = address - imageBase;
        return offset - start < size ? static_cast<DWORD>(offset) : 0;
    }
};

CodeRange gridRendererCode;
CodeRange gridModelCode;
bool gridModelVerified;

constexpr auto gridModelHash2325 = L"F475EB637439598554F2CD49207DC04D9CD05547D49C541CCC9ACB8798843780";
constexpr std::array<DWORD, 2> gridModelReturns2325{0x262D2E, 0x262DF9};

CodeRange moduleCode(HMODULE module)
{
    if (!module) return {};
    const auto* image = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    return {reinterpret_cast<ULONG_PTR>(module), nt->OptionalHeader.BaseOfCode, nt->OptionalHeader.SizeOfCode};
}

void recordGridRendererCandidates(const ULONG_PTR* stack, size_t end, bool onePixel)
{
    bool found = false;
    AcquireSRWLockExclusive(&traceLock);
    for (size_t offset = 0x58; offset < end; offset += sizeof(*stack)) {
        const auto rva = gridRendererCode.rva(stack[offset / sizeof(*stack)]);
        if (!rva) continue;
        DWORD moxelCaller = 0;
        const auto& profile = *gridPlatform;
        const auto parentOffset = offset + profile.wrapperFrameSize;
        if (rva == profile.wrapperReturn && parentOffset < end)
            moxelCaller = gridModelCode.rva(stack[parentOffset / sizeof(*stack)]);
        found = true;
        GridRendererSample* result = nullptr;
        for (auto& entry : trace.rendererCandidates) {
            if (entry.calls && (entry.stackOffset != offset || entry.rva != rva ||
                                entry.moxelCaller != moxelCaller)) continue;
            result = &entry;
            break;
        }
        if (!result) {
            if (trace.otherRenderers != MAXDWORD) ++trace.otherRenderers;
            continue;
        }
        result->stackOffset = static_cast<DWORD>(offset);
        result->rva = rva;
        result->moxelCaller = moxelCaller;
        if (result->calls != MAXDWORD) ++result->calls;
        if (onePixel && result->onePixel != MAXDWORD) ++result->onePixel;
    }
    if (!found && trace.noRenderer != MAXDWORD) ++trace.noRenderer;
    ReleaseSRWLockExclusive(&traceLock);
}

TraceOutcome gridRendererReturn(const ULONG_PTR* stack, GridEvent event, DWORD& rendererReturn)
{
    const auto& profile = *gridPlatform;
    const bool rectangle = event == GridEvent::Rectangle;
    const bool mask = event == GridEvent::Mask;
    const auto expectedTop = rectangle ? profile.rectangleReturn :
        (mask ? tracePlatform->rgbaReturn : profile.solidReturn);
    if (stack[0] != graphicsBase + expectedTop) return TraceOutcome::Caller;

    const size_t colorFrame = mask ? 0x40 : 0;
    const size_t fillFrame = rectangle ? 0 : 0xA0 + colorFrame;
    const size_t rendererOffset = profile.rendererStackOffset + fillFrame;
    const auto stackTop = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    if (address > stackTop || stackTop - address < rendererOffset + sizeof(*stack)) return TraceOutcome::Bounds;

    if (mask && stack[colorFrame / sizeof(*stack)] != graphicsBase + profile.solidReturn)
        return TraceOutcome::Graphics;
    if (!rectangle && stack[fillFrame / sizeof(*stack)] != graphicsBase + profile.fillReturn)
        return TraceOutcome::Graphics;
    if (stack[(fillFrame + 0x50) / sizeof(*stack)] != graphicsBase + profile.dispatchReturn)
        return TraceOutcome::Graphics;

    const auto renderer = stack[rendererOffset / sizeof(*stack)] - frameBase;
    if (renderer != profile.rendererReturns[0] && renderer != profile.rendererReturns[1]) {
        if (!gridModelVerified || renderer != profile.wrapperReturn) return TraceOutcome::Renderer;
        const size_t parentOffset = rendererOffset + profile.wrapperFrameSize;
        if (stackTop - address < parentOffset + sizeof(*stack)) return TraceOutcome::Bounds;
        const auto parent = gridModelCode.rva(stack[parentOffset / sizeof(*stack)]);
        if (parent != gridModelReturns2325[0] && parent != gridModelReturns2325[1])
            return TraceOutcome::Renderer;
    }
    rendererReturn = static_cast<DWORD>(renderer);
    return TraceOutcome::Match;
}

template <typename Sample, typename Equal>
void addGridSample(std::array<Sample, 16>& samples, DWORD& overflow, const Sample& sample, Equal equal)
{
    AcquireSRWLockExclusive(&traceLock);
    DWORD* count = &overflow;
    for (auto& entry : samples) {
        if (entry.calls && (entry.rendererReturn != sample.rendererReturn || !equal(entry, sample))) continue;
        if (!entry.calls) entry = sample;
        count = &entry.calls;
        break;
    }
    if (*count != MAXDWORD) ++*count;
    ReleaseSRWLockExclusive(&traceLock);
}

TraceOutcome recordGridColor(const ULONG_PTR* stack, GridEvent event, double red, double green,
                             double blue, double alpha, DWORD& rendererReturn)
{
    GridColorSample sample{};
    const auto outcome = gridRendererReturn(stack, event, sample.rendererReturn);
    if (outcome != TraceOutcome::Match) return outcome;
    rendererReturn = sample.rendererReturn;
    if (!packRgba(red, green, blue, alpha, sample.rgba)) return TraceOutcome::InvalidSample;
    sample.mask = event == GridEvent::Mask;
    addGridSample(trace.colors, trace.otherColors, sample, [](const auto& a, const auto& b) {
        return a.rgba == b.rgba && a.mask == b.mask;
    });
    return TraceOutcome::Match;
}

TraceOutcome recordGridRectangle(const ULONG_PTR* stack, double x, double y, double width, double height)
{
    GridRectangleSample sample{{x, y, width, height}};
    const auto outcome = gridRendererReturn(stack, GridEvent::Rectangle, sample.rendererReturn);
    if (outcome == TraceOutcome::Renderer) {
        const auto stackTop = reinterpret_cast<ULONG_PTR>(
            reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
        const auto remaining = stackTop - reinterpret_cast<ULONG_PTR>(stack);
        const size_t end = gridPlatform->rendererStackOffset + 0x60;
        recordGridRendererCandidates(stack, remaining < end ? remaining : end, width == 1 || height == 1);
    }
    if (outcome != TraceOutcome::Match) return outcome;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height))
        return TraceOutcome::InvalidSample;
    addGridSample(trace.rectangles, trace.otherRectangles, sample, [](const auto& a, const auto& b) {
        return a.rectangle == b.rectangle;
    });
    return TraceOutcome::Match;
}

void cairoRectangle(void* context, double x, double y, double width, double height)
{
    using DrawRectangle = void (*)(void*, double, double, double, double);
    if (enabled.load(std::memory_order_relaxed)) {
        countOutcome(2, recordGridRectangle(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()),
                                            x, y, width, height));
    }
    reinterpret_cast<DrawRectangle>(hooks[2].original)(context, x, y, width, height);
}
#endif

void setSourceRgb(void* context, double red, double green, double blue)
{
#ifdef THEME_GRID_TRACE
    if (enabled.load(std::memory_order_relaxed)) {
        DWORD rendererReturn{};
        const auto outcome = recordGridColor(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()),
                                             GridEvent::Rgb, red, green, blue, 1.0, rendererReturn);
        countOutcome(0, outcome);
#ifdef THEME_GRID_COLOR_TEST
        if (outcome == TraceOutcome::Match && rendererReturn == gridPlatform->wrapperReturn) {
            red = 0.20;
            green = 0.42;
            blue = 0.65;
        }
#endif
    }
#elif defined(THEME_OUTLINE_TRACE)
    if (enabled.load(std::memory_order_relaxed)) {
        countOutcome(0, recordOutline(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()),
                                      false, red, green, blue, 1.0));
    }
#else
    // The verified grphcs function ends in a tail jump through this IAT slot.
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + solidFillReturn &&
        red == 1.0 && green == 1.0 && blue == 1.0 &&
        enabled.load(std::memory_order_relaxed) &&
        isTreeFill(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()))) {
        red = 0x1F / 255.0;
        green = 0x29 / 255.0;
        blue = 0x38 / 255.0;
    }
#endif
    reinterpret_cast<SetSourceRgb>(hooks[0].original)(context, red, green, blue);
}

void setSourceRgba(void* context, double red, double green, double blue, double alpha)
{
#ifdef THEME_GRID_TRACE
    if (enabled.load(std::memory_order_relaxed)) {
        DWORD rendererReturn{};
        countOutcome(1, recordGridColor(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()),
                                        GridEvent::Mask, red, green, blue, alpha, rendererReturn));
    }
#elif defined(THEME_OUTLINE_TRACE)
    if (enabled.load(std::memory_order_relaxed)) {
        countOutcome(1, recordOutline(static_cast<const ULONG_PTR*>(_AddressOfReturnAddress()),
                                      true, red, green, blue, alpha));
    }
#else
#ifdef THEME_TOOLTIP_TRACE
    const auto expectedReturn = tracePlatform->graphics.rgbaReturn;
#elif defined(THEME_FORM_LINE_TRACE)
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
#elif defined(THEME_FORM_LINE_TRACE)
        recordFormLine(stack, red, green, blue, alpha);
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
#endif
    reinterpret_cast<SetSourceRgba>(hooks[1].original)(context, red, green, blue, alpha);
}

DWORD resolveHooks()
{
#ifdef THEME_FORM_LINE_TRACE
    const auto frame = GetModuleHandleW(L"frmcore.dll");
#elif defined(THEME_SPREADSHEET_TRACE)
    const auto frame = GetModuleHandleW(L"moxelui.dll");
#else
    const auto frame = GetModuleHandleW(L"frame.dll");
#endif
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!frame || !graphics || !cairo) return ERROR_MOD_NOT_FOUND;
#ifdef THEME_TOOLTIP_TRACE
    if (IsDebuggerPresent()) return ERROR_REVISION_MISMATCH;
    const auto* platform = findTooltipTracePlatform(modulePath(nullptr));
    if (!platform || !matchesSha256(modulePath(frame), platform->rendererHash) ||
        !matchesSha256(modulePath(graphics), platform->graphicsHash)) return ERROR_REVISION_MISMATCH;
    tracePlatform = platform;
    hooks[firstHook].offset = platform->graphics.rgbaSlot;
#elif defined(THEME_FORM_LINE_TRACE)
    if (IsDebuggerPresent()) return ERROR_REVISION_MISMATCH;
    const auto* platform = findFormLineTracePlatform(modulePath(nullptr));
    if (!platform || !matchesSha256(modulePath(frame), platform->rendererHash) ||
        !matchesSha256(modulePath(graphics), platform->graphicsHash)) return ERROR_REVISION_MISMATCH;
    tracePlatform = platform;
    hooks[firstHook].offset = platform->rgbaSlot;
#elif defined(THEME_SPREADSHEET_TRACE)
    if (IsDebuggerPresent()) return ERROR_REVISION_MISMATCH;
    const auto* platform = findOutlineTracePlatform(modulePath(nullptr));
    if (!platform || !matchesSha256(modulePath(frame), platform->rendererHash) ||
        !matchesSha256(modulePath(graphics), platform->client.graphicsHash)) return ERROR_REVISION_MISMATCH;
    tracePlatform = platform;
    hooks[0].offset = platform->rgbSlot;
    hooks[1].offset = platform->client.rgbaSlot;
#ifdef THEME_GRID_TRACE
    gridPlatform = &gridTracePlatforms[platform - outlineTracePlatforms];
    hooks[2].offset = gridPlatform->rectangleSlot;
    gridRendererCode = moduleCode(frame);
    const auto model = GetModuleHandleW(L"moxel.dll");
    gridModelCode = moduleCode(model);
    gridModelVerified = gridPlatform->wrapperReturn && model &&
        matchesSha256(modulePath(model), gridModelHash2325);
#ifdef THEME_GRID_COLOR_TEST
    if (!gridModelVerified || !matchesSha256(modulePath(nullptr), platform->client.thickClientHash))
        return ERROR_REVISION_MISMATCH;
#endif
#endif
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
    auto low = hooks[firstHook].offset;
    auto high = low;
    for (size_t i = firstHook + 1; i < hookCount; ++i) {
        if (hooks[i].offset < low) low = hooks[i].offset;
        if (hooks[i].offset > high) high = hooks[i].offset;
    }
    auto* address = reinterpret_cast<PVOID*>(graphicsBase + low);
    const auto size = high - low + sizeof(PVOID);
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
#elif defined(THEME_FORM_LINE_TRACE)
extern "C" __declspec(dllexport) DWORD WINAPI ReadFormLineTrace(LPVOID output)
#elif defined(THEME_OUTLINE_TRACE)
extern "C" __declspec(dllexport) DWORD WINAPI ReadOutlineTraceV2(LPVOID output)
#elif defined(THEME_GRID_TRACE)
extern "C" __declspec(dllexport) DWORD WINAPI ReadGridTraceV4(LPVOID output)
#endif
#ifdef THEME_CAIRO_TRACE
{
    if (!output) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&traceLock);
#ifdef THEME_SPREADSHEET_TRACE
    for (size_t hook = 0; hook < stageCounts.size(); ++hook) {
        for (size_t stage = 0; stage < stageCounts[hook].size(); ++stage) {
            trace.stages[hook][stage] = stageCounts[hook][stage].exchange(0, std::memory_order_relaxed);
        }
    }
#endif
    *static_cast<decltype(trace)*>(output) = trace;
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
