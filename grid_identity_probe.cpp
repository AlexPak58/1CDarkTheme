#include "grid_identity_probe.hpp"
#include "list_identity.hpp"

#include <atomic>
#include <cstring>
#include <intrin.h>
#include <new>

namespace {

constexpr ULONGLONG captureMs = 15000;
constexpr ULONGLONG drainMs = 100;
constexpr ULONG_PTR movLength = 3;
constexpr char breakpoint = static_cast<char>(0xCC);
constexpr char originalOpcode = static_cast<char>(gridProbeSignature.front());

SRWLOCK runLock = SRWLOCK_INIT;
SRWLOCK sampleLock = SRWLOCK_INIT;
std::atomic<ULONG_PTR> handlerAddress{};
std::atomic<ULONGLONG> deadline{};
std::atomic<ULONGLONG> busyDrops{};
std::atomic<DWORD> activeCollectors{};
bool attempted{}; // Protected by runLock.
DWORD sampleCount{};
ULONGLONG capacityDrops{};
ULONGLONG otherCallerCalls{};
ULONGLONG stackReadFailures{};
std::array<GridProbeSample, gridProbeCapacity> samples{};

static_assert(decltype(handlerAddress)::is_always_lock_free);
static_assert(decltype(deadline)::is_always_lock_free);
static_assert(decltype(busyDrops)::is_always_lock_free);
static_assert(decltype(activeCollectors)::is_always_lock_free);

bool containsBytes(const MEMORY_BASIC_INFORMATION& region, ULONG_PTR address, SIZE_T size)
{
    const auto base = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return address >= base && address - base <= region.RegionSize &&
        size <= region.RegionSize - (address - base);
}

int readFaultFilter(DWORD error)
{
    return error == EXCEPTION_ACCESS_VIOLATION || error == EXCEPTION_IN_PAGE_ERROR ||
        error == EXCEPTION_GUARD_PAGE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool guardedCopy(void* destination, const void* source, SIZE_T size)
{
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
}

bool addOffset(ULONG_PTR base, ULONG_PTR offset, ULONG_PTR& address)
{
    if (!base || offset > (std::numeric_limits<ULONG_PTR>::max)() - base) return false;
    address = base + offset;
    return true;
}

bool readableMemory(DWORD protection)
{
    switch (protection) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool readPointer(ULONG_PTR base, ULONG_PTR offset, ULONG_PTR& value)
{
    ULONG_PTR address{};
    MEMORY_BASIC_INFORMATION region{};
    if (!addOffset(base, offset, address) || address % alignof(ULONG_PTR) ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || !readableMemory(region.Protect) ||
        !containsBytes(region, address, sizeof(value))) return false;
    return guardedCopy(&value, reinterpret_cast<LPCVOID>(address), sizeof(value));
}

GridTableIdentity readTable(ULONG_PTR object)
{
    if (!object) return {0, 0, GridTableState::Null};
    ULONG_PTR table{};
    if (!readPointer(object, 0, table)) return {0, 0, GridTableState::ReadFailed};
    MEMORY_BASIC_INFORMATION region{};
    if (!table || table % alignof(ULONG_PTR) ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(table), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.Protect != PAGE_READONLY ||
        !containsBytes(region, table, sizeof(ULONG_PTR)))
        return {0, 0, GridTableState::UnmappedTable};
    const auto base = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    if (table < base || table - base > MAXDWORD) return {0, 0, GridTableState::UnmappedTable};
    return {base, static_cast<DWORD>(table - base), GridTableState::ImageTable};
}

GridTableIdentity readInterface(ULONG_PTR control, ULONG_PTR offset, ULONG_PTR& object)
{
    return readPointer(control, offset, object) ? readTable(object) :
        GridTableIdentity{0, 0, GridTableState::ReadFailed};
}

GridIdentity captureIdentity(ULONG_PTR control, ULONG_PTR frame)
{
    GridIdentity identity{};
    identity.root = readTable(control);
    if (identity.root.state != GridTableState::ImageTable || identity.root.imageBase != frame) return identity;
    if (identity.root.rva == gridStandaloneTableRva) identity.kind = GridRootKind::Standalone;
    else if (identity.root.rva == gridAggregatedTableRva) identity.kind = GridRootKind::Aggregated;
    else return identity;

    ULONG_PTR provider{}, rows{}, site{};
    identity.provider = readInterface(control, gridProviderField, provider);
    identity.rows = readInterface(control, gridRowsField, rows);
    if (identity.kind == GridRootKind::Standalone)
        identity.site = readInterface(control, gridSiteField, site);
    if (provider && rows) {
        ULONG_PTR aliasedRows{};
        if (provider == rows) identity.alias = GridRowsAlias::Same;
        else if (addOffset(provider, gridRowsAliasOffset, aliasedRows) && rows == aliasedRows)
            identity.alias = GridRowsAlias::ProviderPlusOffset;
        else identity.alias = GridRowsAlias::Separate;
    }
    return identity;
}

bool readWrapperReturn(ULONG_PTR rsp, ULONG_PTR& caller)
{
    ULONG_PTR address{};
    ULONG_PTR low{}, high{};
    GetCurrentThreadStackLimits(&low, &high);
    if (!addOffset(rsp, gridWrapperStackOffset, address) || rsp < low || address < low ||
        address >= high || sizeof(caller) > high - address) return false;
    return readPointer(address, 0, caller);
}

void recordCall(const CONTEXT& context, ULONG_PTR frame)
{
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ULONG_PTR caller{};
    if (!readWrapperReturn(context.Rsp, caller)) ++stackReadFailures;
    else if (caller != frame + gridWrapperReturnRva) ++otherCallerCalls;
    else {
        const auto identity = captureIdentity(context.Rcx, frame);
        size_t i{};
        for (; i < sampleCount; ++i) {
            auto& sample = samples[i];
            if (sample.identity == identity) {
                ++sample.calls;
                break;
            }
        }
        if (i == sampleCount) {
            if (sampleCount < samples.size()) samples[sampleCount++] = {identity, 1};
            else ++capacityDrops;
        }
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

void countCall(const CONTEXT& context, ULONG_PTR frame)
{
    const auto until = deadline.load();
    if (!until || GetTickCount64() >= until) return;
    // Check admission again after registering. During drain, a delayed entrant
    // either already counts as active or must observe the closed deadline.
    activeCollectors.fetch_add(1);
    if (deadline.load() && GetTickCount64() < until) recordCall(context, frame);
    activeCollectors.fetch_sub(1);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = handlerAddress.load(std::memory_order_acquire);
    if (!address || reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) != address)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto lastError = GetLastError();
    auto& context = *exception->ContextRecord;
    context.Rdi = context.Rcx; // 48 8B F9: mov rdi, rcx; does not modify flags.
    context.Rip = address + movLength;
    countCall(context, address - gridProbeRva);
    SetLastError(lastError);
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

bool signatureMatches(const BYTE* address)
{
    std::array<BYTE, gridProbeSignature.size()> bytes{};
    return guardedCopy(bytes.data(), address, bytes.size()) && bytes == gridProbeSignature;
}

bool readableExecutable(DWORD protect)
{
    // Modifiers such as PAGE_GUARD are intentionally rejected.
    return protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;
}

DWORD checkImageSite(PatchSite& site, HMODULE frame)
{
    const auto base = reinterpret_cast<ULONG_PTR>(frame);
    if (!base || base > std::numeric_limits<ULONG_PTR>::max() - gridProbeRva)
        return ERROR_INVALID_ADDRESS;
    const auto address = base + gridProbeRva;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)))
        return lastErrorOr(ERROR_INVALID_ADDRESS);
    if (region.AllocationBase != frame || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        !readableExecutable(region.Protect) || !containsBytes(region, address, gridProbeSignature.size()))
        return ERROR_INVALID_ADDRESS;
    // The signature must fit on the page whose protection we change.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (!system.dwPageSize || gridProbeSignature.size() > system.dwPageSize - address % system.dwPageSize)
        return ERROR_INVALID_ADDRESS;
    site.address = reinterpret_cast<BYTE*>(address);
    site.originalProtection = region.Protect;
    return !signatureMatches(site.address) ?
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
        !matchesSha256(modulePath(frontend.module), listFrontendHash)) return ERROR_REVISION_MISMATCH;
    if (const auto error = checkImageSite(site, frame.module)) return error;
    if (IsDebuggerPresent()) return ERROR_BUSY;

    // A trap fetched before rollback may still be delivered after the byte is restored.
    // Keep both code images and this address-specific handler alive until process exit.
    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frame.module), &pinned) ||
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
        !signatureMatches(site.address) ||
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

DWORD finishPatch(PatchSite& site, GridProbeTrace& trace)
{
    const auto error = rollback(site);
    // Give transient failures one bounded recovery attempt; still disclose the error.
    if (site.ownsBreakpoint || site.needsFlush || site.restoreProtection) rollback(site);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(site.address, &region, sizeof(region)) && region.State == MEM_COMMIT &&
        readableExecutable(region.Protect) &&
        containsBytes(region, reinterpret_cast<ULONG_PTR>(site.address), gridProbeSignature.size()))
        trace.finalProtection = region.Protect;
    if (error) return error;
    if (trace.finalProtection != site.originalProtection ||
        !signatureMatches(site.address))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD copySamples(GridProbeTrace& trace)
{
    const auto giveUp = GetTickCount64() + drainMs;
    while (activeCollectors.load() || !TryAcquireSRWLockExclusive(&sampleLock)) {
        if (GetTickCount64() >= giveUp) return ERROR_TIMEOUT;
        Sleep(1);
    }
    trace.sampleCount = sampleCount;
    trace.samples = samples;
    trace.droppedCalls = capacityDrops + busyDrops.load(std::memory_order_relaxed);
    trace.otherCallerCalls = otherCallerCalls;
    trace.stackReadFailures = stackReadFailures;
    trace.totalCalls = trace.droppedCalls + trace.otherCallerCalls + trace.stackReadFailures;
    for (size_t i = 0; i < sampleCount; ++i) trace.totalCalls += samples[i].calls;
    ReleaseSRWLockExclusive(&sampleLock);
    return ERROR_SUCCESS;
}

DWORD runProbe(GridProbeTrace& trace)
{
    if (attempted) return trace.patchError = ERROR_ALREADY_EXISTS;
    attempted = true;
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
        deadline.store(end);
        ULONGLONG now{};
        while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
        deadline.store(0);
        const auto elapsed = now - start;
        trace.elapsedMs = elapsed > MAXDWORD ? MAXDWORD : static_cast<DWORD>(elapsed);
    }
    trace.cleanupError = finishPatch(site, trace);
    if (!trace.patchError) trace.patchError = copySamples(trace);
    return trace.cleanupError ? trace.cleanupError : trace.patchError;
}

bool validInput(const void* output)
{
    if (!output || reinterpret_cast<ULONG_PTR>(output) % alignof(GridProbeTrace)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(GridProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
    return header[0] == sizeof(GridProbeTrace) && header[1] == gridProbeVersion;
}

DWORD publish(void* output, const GridProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (readFaultFilter(GetExceptionCode())) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI GridIdentityProbeAbiV1(void*)
{
    return gridProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunGridIdentityProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    GridProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
