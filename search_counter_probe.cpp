#include "search_counter_probe.hpp"

#include <atomic>
#include <cstring>
#include <intrin.h>
#include <new>

namespace {

constexpr auto frontendHash = L"1E309B705B04DC8A85DC99212368AEF848BCD70AB1BD9F1DB1F09E6A9452531E";
constexpr ULONGLONG captureMs = 15000;
constexpr ULONGLONG drainMs = 100;
constexpr ULONG_PTR movLength = 3;
constexpr char breakpoint = static_cast<char>(0xCC);
constexpr char originalOpcode = static_cast<char>(counterProbeSignature.front());

SRWLOCK runLock = SRWLOCK_INIT;
SRWLOCK sampleLock = SRWLOCK_INIT;
std::atomic<ULONG_PTR> handlerAddress{};
std::atomic<ULONGLONG> deadline{};
std::atomic<DWORD> busyDrops{};
DWORD sampleCount{};
DWORD capacityDrops{};
std::array<CounterProbeSample, counterProbeCapacity> samples{};

static_assert(decltype(handlerAddress)::is_always_lock_free);
static_assert(decltype(deadline)::is_always_lock_free);
static_assert(decltype(busyDrops)::is_always_lock_free);

void countCall(const CONTEXT& context)
{
    const auto until = deadline.load(std::memory_order_acquire);
    if (!until || GetTickCount64() >= until) return;
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (deadline.load(std::memory_order_acquire) && GetTickCount64() < until) {
        const auto thread = GetCurrentThreadId();
        size_t i{};
        for (; i < sampleCount; ++i) {
            auto& sample = samples[i];
            if (sample.receiver == context.Rdi && sample.table == context.Rax && sample.thread == thread) {
                ++sample.calls;
                break;
            }
        }
        if (i == sampleCount) {
            if (sampleCount < samples.size()) samples[sampleCount++] = {context.Rdi, context.Rax, thread, 1};
            else ++capacityDrops;
        }
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = handlerAddress.load(std::memory_order_acquire);
    if (!address || reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) != address)
        return EXCEPTION_CONTINUE_SEARCH;

    auto& context = *exception->ContextRecord;
    countCall(context);
    context.Rcx = context.Rdi; // 48 8B CF: mov rcx, rdi
    context.Rip = address + movLength; // Resume at the original indirect call.
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct PatchSite {
    BYTE* address{};
    DWORD originalProtection{};
    DWORD restoreProtection{};
    bool ownsBreakpoint{};
    bool needsFlush{};
};

DWORD lastErrorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

bool containsBytes(const MEMORY_BASIC_INFORMATION& region, ULONG_PTR address, SIZE_T size)
{
    const auto base = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return address >= base && address - base <= region.RegionSize &&
        size <= region.RegionSize - (address - base);
}

bool readableExecutable(DWORD protect)
{
    // Modifiers such as PAGE_GUARD are intentionally rejected.
    return protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;
}

DWORD checkImageSite(PatchSite& site, HMODULE frontend)
{
    const auto base = reinterpret_cast<ULONG_PTR>(frontend);
    if (!base || base > std::numeric_limits<ULONG_PTR>::max() - counterProbeRva)
        return ERROR_INVALID_ADDRESS;
    const auto address = base + counterProbeRva;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)))
        return lastErrorOr(ERROR_INVALID_ADDRESS);
    if (region.AllocationBase != frontend || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        !readableExecutable(region.Protect) || !containsBytes(region, address, counterProbeSignature.size()))
        return ERROR_INVALID_ADDRESS;
    // The signature must fit on the page whose protection we change.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (!system.dwPageSize || counterProbeSignature.size() > system.dwPageSize - address % system.dwPageSize)
        return ERROR_INVALID_ADDRESS;
    site.address = reinterpret_cast<BYTE*>(address);
    site.originalProtection = region.Protect;
    return std::memcmp(site.address, counterProbeSignature.data(), counterProbeSignature.size()) ?
        ERROR_INVALID_DATA : ERROR_SUCCESS;
}

struct ModuleRef {
    HMODULE module{};
    ~ModuleRef() { if (module) FreeLibrary(module); }
    bool acquire(const wchar_t* name) { return GetModuleHandleExW(0, name, &module) != FALSE; }
};

DWORD prepare(PatchSite& site)
{
    ModuleRef frame, graphics, frontend;
    if (!frame.acquire(L"frame.dll") || !graphics.acquire(L"grphcs.dll") || !frontend.acquire(L"frntend.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.module), frameHash) ||
        !matchesSha256(modulePath(graphics.module), graphicsHash) ||
        !matchesSha256(modulePath(frontend.module), frontendHash)) return ERROR_REVISION_MISMATCH;
    if (const auto error = checkImageSite(site, frontend.module)) return error;
    if (IsDebuggerPresent()) return ERROR_BUSY;

    // A trap fetched before rollback may still be delivered after the byte is restored.
    // Keep both code images and this address-specific handler alive until process exit.
    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frontend.module), &pinned) ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return lastErrorOr(ERROR_DLL_INIT_FAILED);
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) return ERROR_DLL_INIT_FAILED;
    handlerAddress.store(reinterpret_cast<ULONG_PTR>(site.address), std::memory_order_release);
    return ERROR_SUCCESS;
}

DWORD makeWritable(PatchSite& site)
{
    if (site.restoreProtection) return ERROR_SUCCESS;
    DWORD previous{};
    if (!VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &previous))
        return lastErrorOr(ERROR_ACCESS_DENIED);
    site.restoreProtection = previous;
    return ERROR_SUCCESS;
}

DWORD flushAndProtect(PatchSite& site)
{
    DWORD error{};
    if (site.needsFlush) {
        if (FlushInstructionCache(GetCurrentProcess(), site.address, 1)) site.needsFlush = false;
        else error = lastErrorOr(ERROR_WRITE_FAULT);
    }
    if (site.restoreProtection) {
        DWORD ignored{};
        if (VirtualProtect(site.address, 1, site.restoreProtection, &ignored)) site.restoreProtection = 0;
        else if (!error) error = lastErrorOr(ERROR_ACCESS_DENIED);
    }
    return error;
}

DWORD install(PatchSite& site)
{
    if (const auto error = makeWritable(site)) return error;
    DWORD error{};
    auto* opcode = reinterpret_cast<volatile char*>(site.address);
    if (site.restoreProtection != site.originalProtection ||
        std::memcmp(site.address, counterProbeSignature.data(), counterProbeSignature.size()) ||
        _InterlockedCompareExchange8(opcode, breakpoint, originalOpcode) != originalOpcode) {
        error = ERROR_INVALID_DATA;
    } else {
        site.ownsBreakpoint = true;
        site.needsFlush = true;
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD rollback(PatchSite& site)
{
    DWORD error{};
    if (site.ownsBreakpoint) {
        if (const auto writableError = makeWritable(site)) return writableError;
        auto* opcode = reinterpret_cast<volatile char*>(site.address);
        if (_InterlockedCompareExchange8(opcode, originalOpcode, breakpoint) != breakpoint)
            error = ERROR_INVALID_DATA; // Do not overwrite a different owner's byte.
        site.ownsBreakpoint = false;
        site.needsFlush = true; // Also invalidate any previously cached trap on conflict.
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD finishPatch(PatchSite& site, CounterProbeTrace& trace)
{
    const auto error = rollback(site);
    // Give transient failures one bounded recovery attempt; still disclose the error.
    if (site.ownsBreakpoint || site.needsFlush || site.restoreProtection) rollback(site);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(site.address, &region, sizeof(region))) trace.finalProtection = region.Protect;
    if (error) return error;
    if (trace.finalProtection != site.originalProtection ||
        std::memcmp(site.address, counterProbeSignature.data(), counterProbeSignature.size()))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD copySamples(CounterProbeTrace& trace)
{
    const auto giveUp = GetTickCount64() + drainMs;
    while (!TryAcquireSRWLockExclusive(&sampleLock)) {
        if (GetTickCount64() >= giveUp) return ERROR_TIMEOUT;
        Sleep(1);
    }
    trace.sampleCount = sampleCount;
    trace.samples = samples;
    trace.droppedCalls = capacityDrops + busyDrops.load(std::memory_order_relaxed);
    trace.totalCalls = trace.droppedCalls;
    for (size_t i = 0; i < sampleCount; ++i) trace.totalCalls += samples[i].calls;
    ReleaseSRWLockExclusive(&sampleLock);
    return ERROR_SUCCESS;
}

DWORD runProbe(CounterProbeTrace& trace)
{
    if (handlerAddress.load(std::memory_order_acquire)) return trace.patchError = ERROR_ALREADY_EXISTS;
    if (IsDebuggerPresent()) return trace.patchError = ERROR_BUSY;
    PatchSite site;
    try {
        trace.patchError = prepare(site);
    } catch (const std::bad_alloc&) {
        trace.patchError = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (trace.patchError) return trace.patchError;

    trace.originalProtection = site.originalProtection;
    trace.patchError = IsDebuggerPresent() ? ERROR_BUSY : install(site);
    if (!trace.patchError) {
        const auto start = GetTickCount64();
        const auto end = start + captureMs;
        deadline.store(end, std::memory_order_release);
        ULONGLONG now{};
        while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
        deadline.store(0, std::memory_order_release);
        const auto elapsed = now - start;
        trace.elapsedMs = elapsed > MAXDWORD ? MAXDWORD : static_cast<DWORD>(elapsed);
    }
    trace.cleanupError = finishPatch(site, trace);
    if (!trace.patchError) trace.patchError = copySamples(trace);
    return trace.cleanupError ? trace.cleanupError : trace.patchError;
}

bool validInput(const void* output)
{
    if (!output) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(CounterProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return header[0] == sizeof(CounterProbeTrace) && header[1] == counterProbeVersion;
}

DWORD publish(void* output, const CounterProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI SearchCounterProbeAbiV1(void*)
{
    return counterProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunSearchCounterProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    CounterProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
