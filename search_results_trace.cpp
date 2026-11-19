#include "search_results_trace.hpp"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace {

constexpr ULONG_PTR dispatchRva = 0x82F40D;
constexpr ULONG_PTR copyRva = 0x2FAD04;
constexpr ULONG_PTR resultsGridVtableRva = 0xA88AC0;
constexpr ULONG_PTR standaloneGridVtableRva = 0xA8A278;
constexpr auto frontendHash = L"1E309B705B04DC8A85DC99212368AEF848BCD70AB1BD9F1DB1F09E6A9452531E";
constexpr char breakpoint = static_cast<char>(0xCC);

struct ProbeSite {
    ULONG_PTR rva;
    std::array<BYTE, 8> signature;
    DWORD instructionLength;
    BYTE* address{};
    bool patched{};
    bool needsFlush{};
    bool needsProtectionRestore{};
    DWORD originalProtection{};
};

ProbeSite dispatch{dispatchRva, {0xBA, 0x04, 0, 0, 0, 0xFF, 0x50, 0x48}, 5};
ProbeSite colorCopy{copyRva, {0x8B, 0x48, 0x74, 0x89, 0x4C, 0x24, 0x30, 0x48}, 3};
ProbeSite standalone{0x2EF073, {0x48, 0x8D, 0x05, 0xFE, 0xB1, 0x79, 0, 0x48}, 7};
ProbeSite aggregated{0x2EF42D, {0x48, 0x8D, 0x05, 0x8C, 0x96, 0x79, 0, 0x48}, 7};
ProbeSite background{0x811A00, {0x49, 0x8B, 0xCF, 0xE8, 0xC8, 0x2B, 0x89, 0xFF}, 3};
constexpr std::array<BYTE, 8> whiteWrite{0x41, 0xC7, 0x47, 0x70, 0xFF, 0xFF, 0xFF, 0};
std::array<ProbeSite, searchGridWhiteWriteRvas.size()> whiteSites{
    ProbeSite{searchGridWhiteWriteRvas[0], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[1], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[2], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[3], whiteWrite, 8},
};
const std::array sites{&dispatch, &colorCopy, &whiteSites[0], &whiteSites[1], &whiteSites[2], &whiteSites[3],
                       &standalone, &aggregated, &background};

SRWLOCK probeLock = SRWLOCK_INIT;
ULONG_PTR resultsGridVtable{};
ULONG_PTR standaloneGridVtable{};
ULONG_PTR frontendBase{};
bool handlerInstalled{};
std::atomic_bool collecting{};
std::atomic<DWORD> cachedColorCalls{};
std::atomic<DWORD> originalColorCalls{};
std::atomic<DWORD> unexpectedColorCalls{};
std::atomic<DWORD> otherGridCalls{};
std::atomic<DWORD> cachedColorCopies{};
std::atomic<DWORD> otherGridColorCopies{};
std::array<std::atomic<DWORD>, searchGridWhiteWriteRvas.size()> whiteWrites{};
std::atomic<DWORD> traceGeneration{};
std::atomic<DWORD> resultsStandaloneCreations{};
std::atomic<DWORD> resultsAggregatedCreations{};
std::atomic<DWORD> otherCreations{};
std::atomic<DWORD> resultsBackgrounds{};
std::atomic<DWORD> otherBackgrounds{};
std::atomic<DWORD> replacedCairoFills{};
std::atomic<DWORD> replacedRowFills{};
std::atomic<DWORD> replacedTextBackgrounds{};
std::atomic<DWORD> replacedTexts{};

struct GridCandidate {
    ULONG_PTR address;
    ULONG_PTR vtable;
    DWORD generation;
};

thread_local GridCandidate lastCandidate{};

bool matchesCandidate(ULONG_PTR control)
{
    if (!control || control != lastCandidate.address ||
        lastCandidate.generation != traceGeneration.load(std::memory_order_relaxed)) return false;
    __try {
        return *reinterpret_cast<const ULONG_PTR*>(control) == lastCandidate.vtable;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

#ifdef THEME_SEARCH_RESULTS_COLOR_TEST
using SetSourceRgb = void (*)(void*, double, double, double);
using SetSourceRgba = void (*)(void*, double, double, double, double);
void setSearchSourceRgb(void*, double, double, double);
void setSearchSourceRgba(void*, double, double, double, double);
struct CairoHook {
    ULONG_PTR offset;
    const char* name;
    PVOID replacement;
    PVOID* slot{};
    PVOID original{};
    bool patched{};
};
std::array cairoHooks{
    CairoHook{0xC7B60, "cairo_set_source_rgb", reinterpret_cast<PVOID>(&setSearchSourceRgb)},
    CairoHook{0xC7C20, "cairo_set_source_rgba", reinterpret_cast<PVOID>(&setSearchSourceRgba)},
};
ULONG_PTR paintFrameBase{};
ULONG_PTR graphicsBase{};
DWORD cairoPageProtection{};

bool isSearchReceiver(const ULONG_PTR* stack, size_t paintOffset)
{
    if (stack[paintOffset / 8] != paintFrameBase + 0x8117F5) return false;
    const auto window = stack[(paintOffset - 0x20) / 8];
    return window > 0x100 && matchesCandidate(window - 0x100);
}

bool isSearchBackground(const ULONG_PTR* stack)
{
    const auto top = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    // A45D0 saves the renderer's RDI directly below its return address.
    return address <= top && top - address >= 0x1A8 &&
        stack[0xA0 / 8] == graphicsBase + 0xAC6DC &&
        stack[0x150 / 8] == paintFrameBase + 0xA4638 &&
        stack[0x1A0 / 8] == paintFrameBase + 0x811A08 &&
        matchesCandidate(stack[0x198 / 8]);
}

bool isSearchRow(const ULONG_PTR* stack)
{
    const auto top = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const auto address = reinterpret_cast<ULONG_PTR>(stack);
    if (address > top || top - address < 0x608 ||
        stack[0x220 / 8] != paintFrameBase + 0x303466) return false;
    return isSearchReceiver(stack, 0x600);
}

void setSearchSourceRgb(void* context, double red, double green, double blue)
{
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + 0xAEE56 &&
        red == 1.0 && green == 1.0 && blue == 1.0 &&
        collecting.load(std::memory_order_acquire)) {
        const auto* stack = static_cast<const ULONG_PTR*>(_AddressOfReturnAddress());
        if (isSearchBackground(stack)) {
            red = 0x1F / 255.0;
            green = 0x29 / 255.0;
            blue = 0x38 / 255.0;
            replacedCairoFills.fetch_add(1, std::memory_order_relaxed);
        } else if (isSearchRow(stack)) {
            red = 0x1F / 255.0;
            green = 0x29 / 255.0;
            blue = 0x38 / 255.0;
            replacedRowFills.fetch_add(1, std::memory_order_relaxed);
        }
    }
    reinterpret_cast<SetSourceRgb>(cairoHooks[0].original)(context, red, green, blue);
}

void setSearchSourceRgba(void* context, double red, double green, double blue, double alpha)
{
    if (reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + 0x2336 &&
        collecting.load(std::memory_order_acquire)) {
        const auto* stack = static_cast<const ULONG_PTR*>(_AddressOfReturnAddress());
        const auto top = reinterpret_cast<ULONG_PTR>(
            reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
        const auto address = reinterpret_cast<ULONG_PTR>(stack);
        const auto remaining = address <= top ? top - address : 0;
        if (remaining >= 0xCC8 && stack[0x40 / 8] == graphicsBase + 0x7905B &&
            red == 1.0 && green == 1.0 && blue == 1.0 &&
            stack[0x4C0 / 8] == paintFrameBase + 0x85F475 && isSearchReceiver(stack, 0xCC0)) {
            red = 0x1F / 255.0;
            green = 0x29 / 255.0;
            blue = 0x38 / 255.0;
            replacedTextBackgrounds.fetch_add(1, std::memory_order_relaxed);
        } else if (remaining >= 0xD88 && stack[0x40 / 8] == graphicsBase + 0x79821 &&
                   red == 0.0 && green == 0.0 && blue == 0.0 &&
                   stack[0x580 / 8] == paintFrameBase + 0x85F475 && isSearchReceiver(stack, 0xD80)) {
            red = 0xE5 / 255.0;
            green = 0xEB / 255.0;
            blue = 0xF5 / 255.0;
            replacedTexts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    reinterpret_cast<SetSourceRgba>(cairoHooks[1].original)(context, red, green, blue, alpha);
}

DWORD patchCairo(bool enable)
{
    DWORD error{};
    // Both slots occupy one page. Keep one protection snapshot across rollback/retry.
    auto* address = cairoHooks.front().slot;
    const auto size = cairoHooks.back().offset - cairoHooks.front().offset + sizeof(PVOID);
    for (auto& hook : cairoHooks) {
        if (hook.patched == enable) continue;
        if (!cairoPageProtection &&
            !VirtualProtect(address, size, PAGE_READWRITE, &cairoPageProtection))
            return GetLastError();
        const auto from = enable ? hook.original : hook.replacement;
        const auto to = enable ? hook.replacement : hook.original;
        if (InterlockedCompareExchangePointer(hook.slot, to, from) == from)
            hook.patched = enable;
        else
            error = ERROR_INVALID_DATA;
    }
    if (cairoPageProtection) {
        DWORD ignored{};
        if (VirtualProtect(address, size, cairoPageProtection, &ignored))
            cairoPageProtection = 0;
        else if (!error)
            error = GetLastError();
    }
    return error;
}
#endif

bool hasResultsLoader(const CONTEXT& fault)
{
    ULONG_PTR stackLow{}, stackHigh{};
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    CONTEXT frame = fault;
    // Unwind exact return addresses. Scanning raw stack words can pick up an old frame.
    __try {
        for (int depth = 0; depth < 48 && frame.Rip; ++depth) {
            if (frame.Rip == frontendBase + 0x28B871 || frame.Rip == frontendBase + 0x28B87B ||
                frame.Rip == frontendBase + 0x28B892) return true;
            const auto stack = frame.Rsp;
            if (stack < stackLow || stack > stackHigh - sizeof(ULONG_PTR)) break;
            DWORD64 image{};
            const auto entry = RtlLookupFunctionEntry(frame.Rip, &image, nullptr);
            if (entry) {
                void* data{};
                DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, frame.Rip, entry, &frame, &data,
                                 &establisher, nullptr);
            } else {
                frame.Rip = *reinterpret_cast<const ULONG_PTR*>(stack);
                frame.Rsp += sizeof(ULONG_PTR);
            }
            if (frame.Rsp <= stack) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

void observeCreation(const CONTEXT& context, bool aggregate)
{
    const auto control = context.Rbx + (aggregate ? 0x18 : 0);
    if (lastCandidate.address == control) lastCandidate = {};
    if (!hasResultsLoader(context)) {
        otherCreations.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& count = aggregate ? resultsAggregatedCreations : resultsStandaloneCreations;
    count.fetch_add(1, std::memory_order_relaxed);
    lastCandidate = {control, aggregate ? resultsGridVtable : standaloneGridVtable,
                     traceGeneration.load(std::memory_order_relaxed)};
}

void countDispatch(const CONTEXT& context)
{
    const auto vtable = *reinterpret_cast<const ULONG_PTR*>(context.R15);
    if (vtable != resultsGridVtable)
        otherGridCalls.fetch_add(1, std::memory_order_relaxed);
    else if (context.R8 == context.Rbp + 0xD0)
        cachedColorCalls.fetch_add(1, std::memory_order_relaxed);
    else if (context.R8 == context.R15 + 0x1188)
        originalColorCalls.fetch_add(1, std::memory_order_relaxed);
    else
        unexpectedColorCalls.fetch_add(1, std::memory_order_relaxed);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;

    auto& context = *exception->ContextRecord;
    const auto address = record->ExceptionAddress;
    const bool count = collecting.load(std::memory_order_acquire);
    if (address == standalone.address || address == aggregated.address) {
        const bool aggregate = address == aggregated.address;
        if (count) observeCreation(context, aggregate);
        context.Rax = aggregate ? resultsGridVtable : standaloneGridVtable;
        context.Rip = reinterpret_cast<ULONG_PTR>(address) + standalone.instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == background.address) {
        if (count) {
            const bool match = matchesCandidate(context.Rdi);
            auto& counter = match ? resultsBackgrounds : otherBackgrounds;
            counter.fetch_add(1, std::memory_order_relaxed);
        }
        context.Rcx = context.R15; // mov rcx, r15
        context.Rip = reinterpret_cast<ULONG_PTR>(address) + background.instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == dispatch.address) {
        if (count) countDispatch(context);
        context.Rdx = 4; // mov edx, 4
        context.Rip = reinterpret_cast<ULONG_PTR>(address) + dispatch.instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == colorCopy.address) {
        if (count) {
            const auto grid = *reinterpret_cast<const ULONG_PTR*>(context.R15 + 8);
            const auto vtable = *reinterpret_cast<const ULONG_PTR*>(grid);
            auto& counter = vtable == resultsGridVtable ? cachedColorCopies : otherGridColorCopies;
            counter.fetch_add(1, std::memory_order_relaxed);
        }
        context.Rcx = *reinterpret_cast<const DWORD*>(context.Rax + 0x74); // mov ecx, [rax+74h]
        context.Rip = reinterpret_cast<ULONG_PTR>(address) + colorCopy.instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (size_t i = 0; i < whiteSites.size(); ++i) {
        if (address != whiteSites[i].address) continue;
        *reinterpret_cast<DWORD*>(context.R15 + 0x70) = 0x00FFFFFF; // mov dword ptr [r15+70h], 0FFFFFFh
        if (count) whiteWrites[i].fetch_add(1, std::memory_order_relaxed);
        context.Rip = reinterpret_cast<ULONG_PTR>(address) + whiteSites[i].instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD prepare()
{
    HMODULE frame = GetModuleHandleW(L"frame.dll");
    HMODULE frontend = GetModuleHandleW(L"frntend.dll");
    if (!frame || !frontend) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash) ||
        !matchesSha256(modulePath(frontend), frontendHash)) return ERROR_REVISION_MISMATCH;
    const auto frameBase = reinterpret_cast<ULONG_PTR>(frame);
    frontendBase = reinterpret_cast<ULONG_PTR>(frontend);
    for (auto* site : sites) {
        site->address = reinterpret_cast<BYTE*>(frameBase + site->rva);
        if (std::memcmp(site->address, site->signature.data(), site->signature.size()))
            return ERROR_INVALID_DATA;
        // Snapshot each page before any patch can leave a shared page writable.
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(site->address, &page, sizeof(page))) return GetLastError();
        if (page.State != MEM_COMMIT || !page.Protect) return ERROR_INVALID_ADDRESS;
        site->originalProtection = page.Protect;
    }

    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
#ifdef THEME_SEARCH_RESULTS_COLOR_TEST
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!graphics || !cairo) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(graphics), graphicsHash)) return ERROR_REVISION_MISMATCH;
    for (auto& hook : cairoHooks) {
        hook.original = reinterpret_cast<PVOID>(GetProcAddress(cairo, hook.name));
        if (!hook.original) return ERROR_PROC_NOT_FOUND;
        hook.slot = reinterpret_cast<PVOID*>(reinterpret_cast<ULONG_PTR>(graphics) + hook.offset);
    }
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(graphics), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(cairo), &pinned))
        return GetLastError();
    paintFrameBase = frameBase;
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
#endif
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frame), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frontend), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return GetLastError();

    // The handler remains available for any breakpoint already in flight on disable.
    resultsGridVtable = frameBase + resultsGridVtableRva;
    standaloneGridVtable = frameBase + standaloneGridVtableRva;
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) return ERROR_DLL_INIT_FAILED;
    handlerInstalled = true;
    return ERROR_SUCCESS;
}

DWORD patch(ProbeSite& site, bool enable)
{
    DWORD error{};
    if (site.patched != enable) {
        DWORD ignored{};
        // Another site on this page may have restored protection since our last attempt.
        if (!VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &ignored)) return GetLastError();
        site.needsProtectionRestore = true;
        auto* opcode = reinterpret_cast<volatile char*>(site.address);
        const auto original = static_cast<char>(site.signature.front());
        const auto from = enable ? original : breakpoint;
        const auto to = enable ? breakpoint : original;
        if (_InterlockedCompareExchange8(opcode, to, from) == from) {
            site.patched = enable;
            site.needsFlush = true;
        } else {
            error = ERROR_INVALID_DATA;
        }
    }
    if (site.needsFlush) {
        if (FlushInstructionCache(GetCurrentProcess(), site.address, 1))
            site.needsFlush = false;
        else if (!error)
            error = GetLastError();
    }
    if (site.needsProtectionRestore) {
        DWORD ignored{};
        if (VirtualProtect(site.address, 1, site.originalProtection, &ignored))
            site.needsProtectionRestore = false;
        else if (!error)
            error = GetLastError();
    }
    return error;
}

DWORD disable()
{
    collecting.store(false, std::memory_order_relaxed);
    DWORD firstError{};
#ifdef THEME_SEARCH_RESULTS_COLOR_TEST
    firstError = patchCairo(false);
#endif
    for (auto it = sites.rbegin(); it != sites.rend(); ++it) {
        const DWORD error = patch(**it, false);
        if (!firstError) firstError = error;
    }
    return firstError;
}

DWORD setEnabled(bool enable)
{
    if (!enable) return disable();
    if (collecting.load(std::memory_order_relaxed)) return ERROR_SUCCESS;
    if (IsDebuggerPresent()) return ERROR_BUSY;
    const DWORD resetError = disable();
    if (resetError) return resetError;
    if (!handlerInstalled) {
        const DWORD error = prepare();
        if (error) return error;
    }
    for (const auto* site : sites) {
        if (std::memcmp(site->address, site->signature.data(), site->signature.size()))
            return ERROR_INVALID_DATA;
    }
    for (auto* site : sites) {
        if (const DWORD error = patch(*site, true)) {
            const DWORD rollbackError = disable();
            return rollbackError ? rollbackError : error;
        }
    }
#ifdef THEME_SEARCH_RESULTS_COLOR_TEST
    if (const DWORD error = patchCairo(true)) {
        const DWORD rollbackError = disable();
        return rollbackError ? rollbackError : error;
    }
#endif
    traceGeneration.fetch_add(1, std::memory_order_relaxed);
    collecting.store(true, std::memory_order_release);
    return ERROR_SUCCESS;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SetTreeTheme(void* setting)
{
    if (setting && reinterpret_cast<ULONG_PTR>(setting) != enableTreeOnly)
        return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&probeLock);
    const DWORD error = setEnabled(setting != nullptr);
    ReleaseSRWLockExclusive(&probeLock);
    return error;
}

extern "C" __declspec(dllexport) DWORD WINAPI ReadSearchResultsTraceV6(void* output)
{
    if (!output) return ERROR_INVALID_PARAMETER;
    SearchResultsTrace trace{
        cachedColorCalls.exchange(0, std::memory_order_relaxed),
        originalColorCalls.exchange(0, std::memory_order_relaxed),
        unexpectedColorCalls.exchange(0, std::memory_order_relaxed),
        otherGridCalls.exchange(0, std::memory_order_relaxed),
        cachedColorCopies.exchange(0, std::memory_order_relaxed),
        otherGridColorCopies.exchange(0, std::memory_order_relaxed),
        {},
        resultsStandaloneCreations.exchange(0, std::memory_order_relaxed),
        resultsAggregatedCreations.exchange(0, std::memory_order_relaxed),
        otherCreations.exchange(0, std::memory_order_relaxed),
        resultsBackgrounds.exchange(0, std::memory_order_relaxed),
        otherBackgrounds.exchange(0, std::memory_order_relaxed),
        replacedCairoFills.exchange(0, std::memory_order_relaxed),
        replacedRowFills.exchange(0, std::memory_order_relaxed),
        replacedTextBackgrounds.exchange(0, std::memory_order_relaxed),
        replacedTexts.exchange(0, std::memory_order_relaxed),
    };
    for (size_t i = 0; i < whiteWrites.size(); ++i)
        trace.whiteWrites[i] = whiteWrites[i].exchange(0, std::memory_order_relaxed);
    *static_cast<SearchResultsTrace*>(output) = trace;
    return ERROR_SUCCESS;
}
