#include "search_results_trace.hpp"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace {

constexpr ULONG_PTR dispatchRva = 0x82F40D;
constexpr ULONG_PTR copyRva = 0x2FAD04;
constexpr ULONG_PTR resultsGridVtableRva = 0xA88AC0;
constexpr char breakpoint = static_cast<char>(0xCC);

struct ProbeSite {
    ULONG_PTR rva;
    std::array<BYTE, 8> signature;
    DWORD instructionLength;
    BYTE* address{};
    bool patched{};
    bool needsFlush{};
    DWORD originalProtection{};
};

ProbeSite dispatch{dispatchRva, {0xBA, 0x04, 0, 0, 0, 0xFF, 0x50, 0x48}, 5};
ProbeSite colorCopy{copyRva, {0x8B, 0x48, 0x74, 0x89, 0x4C, 0x24, 0x30, 0x48}, 3};
constexpr std::array<BYTE, 8> whiteWrite{0x41, 0xC7, 0x47, 0x70, 0xFF, 0xFF, 0xFF, 0};
std::array<ProbeSite, searchGridWhiteWriteRvas.size()> whiteSites{
    ProbeSite{searchGridWhiteWriteRvas[0], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[1], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[2], whiteWrite, 8},
    ProbeSite{searchGridWhiteWriteRvas[3], whiteWrite, 8},
};
const std::array sites{&dispatch, &colorCopy, &whiteSites[0], &whiteSites[1], &whiteSites[2], &whiteSites[3]};

SRWLOCK probeLock = SRWLOCK_INIT;
ULONG_PTR resultsGridVtable{};
bool handlerInstalled{};
std::atomic_bool collecting{};
std::atomic<DWORD> cachedColorCalls{};
std::atomic<DWORD> originalColorCalls{};
std::atomic<DWORD> unexpectedColorCalls{};
std::atomic<DWORD> otherGridCalls{};
std::atomic<DWORD> cachedColorCopies{};
std::atomic<DWORD> otherGridColorCopies{};
std::array<std::atomic<DWORD>, searchGridWhiteWriteRvas.size()> whiteWrites{};

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
    const bool count = collecting.load(std::memory_order_relaxed);
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
    if (!frame) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash)) return ERROR_REVISION_MISMATCH;
    const auto frameBase = reinterpret_cast<ULONG_PTR>(frame);
    for (auto* site : sites) {
        site->address = reinterpret_cast<BYTE*>(frameBase + site->rva);
        if (std::memcmp(site->address, site->signature.data(), site->signature.size()))
            return ERROR_INVALID_DATA;
    }

    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frame), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return GetLastError();

    // The handler remains available for any breakpoint already in flight on disable.
    resultsGridVtable = frameBase + resultsGridVtableRva;
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) return ERROR_DLL_INIT_FAILED;
    handlerInstalled = true;
    return ERROR_SUCCESS;
}

DWORD patch(ProbeSite& site, bool enable)
{
    DWORD error{};
    if (site.patched != enable) {
        if (!site.originalProtection &&
            !VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &site.originalProtection))
            return GetLastError();
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
    if (site.originalProtection) {
        DWORD ignored{};
        if (VirtualProtect(site.address, 1, site.originalProtection, &ignored))
            site.originalProtection = 0;
        else if (!error)
            error = GetLastError();
    }
    return error;
}

DWORD disable()
{
    collecting.store(false, std::memory_order_relaxed);
    DWORD firstError{};
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
    collecting.store(true, std::memory_order_relaxed);
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

extern "C" __declspec(dllexport) DWORD WINAPI ReadSearchResultsTraceV2(void* output)
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
    };
    for (size_t i = 0; i < whiteWrites.size(); ++i)
        trace.whiteWrites[i] = whiteWrites[i].exchange(0, std::memory_order_relaxed);
    *static_cast<SearchResultsTrace*>(output) = trace;
    return ERROR_SUCCESS;
}
