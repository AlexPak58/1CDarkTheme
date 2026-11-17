#include "search_results_trace.hpp"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace {

constexpr ULONG_PTR dispatchRva = 0x82F40D;
constexpr ULONG_PTR resultsGridVtableRva = 0xA88AC0;
// mov edx, 4; call qword ptr [rax+48h]
constexpr BYTE originalCode[] = {0xBA, 0x04, 0x00, 0x00, 0x00, 0xFF, 0x50, 0x48};
constexpr char breakpoint = static_cast<char>(0xCC);

SRWLOCK probeLock = SRWLOCK_INIT;
BYTE* dispatch{};
ULONG_PTR resultsGridVtable{};
bool patched{};
std::atomic_bool collecting{};
std::atomic<DWORD> cachedColorCalls{};
std::atomic<DWORD> originalColorCalls{};
std::atomic<DWORD> unexpectedColorCalls{};
std::atomic<DWORD> otherGridCalls{};

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT ||
        record->ExceptionAddress != dispatch) return EXCEPTION_CONTINUE_SEARCH;

    auto& context = *exception->ContextRecord;
    if (collecting.load(std::memory_order_relaxed)) {
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
    // Execute the displaced mov edx, 4. Leave the call and its other arguments intact.
    context.Rdx = 4;
    context.Rip = reinterpret_cast<ULONG_PTR>(dispatch) + 5;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD prepare()
{
    HMODULE frame = GetModuleHandleW(L"frame.dll");
    if (!frame) return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame), frameHash)) return ERROR_REVISION_MISMATCH;
    const auto frameBase = reinterpret_cast<ULONG_PTR>(frame);
    auto* site = reinterpret_cast<BYTE*>(frameBase + dispatchRva);
    if (std::memcmp(site, originalCode, sizeof(originalCode))) return ERROR_INVALID_DATA;

    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(site), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return GetLastError();

    // The handler remains available for any breakpoint already in flight on disable.
    dispatch = site;
    resultsGridVtable = frameBase + resultsGridVtableRva;
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) {
        dispatch = nullptr;
        return ERROR_DLL_INIT_FAILED;
    }
    return ERROR_SUCCESS;
}

DWORD setEnabled(bool enable)
{
    if (!enable) collecting.store(false, std::memory_order_relaxed);
    if (enable == patched)
        return enable && !collecting.load(std::memory_order_relaxed) ? ERROR_BUSY : ERROR_SUCCESS;
    if (enable) {
        if (IsDebuggerPresent()) return ERROR_BUSY;
        if (!dispatch) {
            const DWORD error = prepare();
            if (error) return error;
        }
        if (std::memcmp(dispatch, originalCode, sizeof(originalCode))) return ERROR_INVALID_DATA;
    }

    DWORD protection{};
    if (!VirtualProtect(dispatch, 1, PAGE_EXECUTE_READWRITE, &protection)) return GetLastError();
    auto* opcode = reinterpret_cast<volatile char*>(dispatch);
    const char oldOpcode = enable ? static_cast<char>(originalCode[0]) : breakpoint;
    const char newOpcode = enable ? breakpoint : static_cast<char>(originalCode[0]);
    DWORD error{};
    if (_InterlockedCompareExchange8(opcode, newOpcode, oldOpcode) != oldOpcode) {
        error = ERROR_INVALID_DATA;
    } else {
        patched = enable;
        if (!FlushInstructionCache(GetCurrentProcess(), dispatch, 1)) {
            error = GetLastError();
            if (enable) {
                _InterlockedCompareExchange8(opcode, oldOpcode, newOpcode);
                patched = false;
                FlushInstructionCache(GetCurrentProcess(), dispatch, 1);
            }
        }
    }
    DWORD ignored{};
    if (!VirtualProtect(dispatch, 1, protection, &ignored) && !error)
        error = GetLastError();
    collecting.store(enable && patched && !error, std::memory_order_relaxed);
    return error;
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

extern "C" __declspec(dllexport) DWORD WINAPI ReadSearchResultsTraceV1(void* output)
{
    if (!output) return ERROR_INVALID_PARAMETER;
    *static_cast<SearchResultsTrace*>(output) = {
        cachedColorCalls.exchange(0, std::memory_order_relaxed),
        originalColorCalls.exchange(0, std::memory_order_relaxed),
        unexpectedColorCalls.exchange(0, std::memory_order_relaxed),
        otherGridCalls.exchange(0, std::memory_order_relaxed),
    };
    return ERROR_SUCCESS;
}
