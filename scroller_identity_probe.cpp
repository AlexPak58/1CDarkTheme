#include "scroller_identity_probe.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <new>

namespace {

constexpr ULONGLONG captureMs = 15000;
constexpr ULONGLONG drainMs = 100;
constexpr char breakpoint = static_cast<char>(0xCC);

SRWLOCK runLock = SRWLOCK_INIT;
SRWLOCK sampleLock = SRWLOCK_INIT;
std::array<std::atomic<ULONG_PTR>, scrollerSites.size()> handlerAddresses{};
std::array<std::atomic<bool>, scrollerSites.size()> replaySites{};
std::atomic<ULONGLONG> deadline{};
std::array<std::atomic<ULONGLONG>, scrollerSites.size()> busyDrops{};
std::atomic<DWORD> activeCollectors{};
bool attempted{};
DWORD sampleCount{};
std::array<ScrollerProbeCounts, scrollerSites.size()> counts{};
std::array<ScrollerProbeSample, scrollerProbeCapacity> samples{};

static_assert(std::atomic<ULONG_PTR>::is_always_lock_free);
static_assert(std::atomic<ULONGLONG>::is_always_lock_free);
static_assert(std::atomic<DWORD>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

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

template <typename T>
bool readMemory(ULONG_PTR base, ULONG_PTR offset, T& value)
{
    ULONG_PTR address{};
    MEMORY_BASIC_INFORMATION region{};
    if (!addOffset(base, offset, address) || address % alignof(T) ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || !readableMemory(region.Protect) ||
        !containsBytes(region, address, sizeof(value))) return false;
    return guardedCopy(&value, reinterpret_cast<LPCVOID>(address), sizeof(value));
}

enum class Observation { Accepted, Mismatch, ReadFailure };

ScrollerLocation imageLocation(ULONG_PTR address, bool code)
{
    if (!address) return {0, 0, ScrollerLocationState::Null};
    MEMORY_BASIC_INFORMATION region{};
    const SIZE_T length = code ? 1 : sizeof(ULONG_PTR);
    if ((!code && address % alignof(ULONG_PTR)) ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (code ? region.Protect != PAGE_EXECUTE_READ && region.Protect != PAGE_EXECUTE :
                region.Protect != PAGE_READONLY) || !containsBytes(region, address, length))
        return {0, 0, ScrollerLocationState::Unmapped};
    const auto base = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    if (!base || address < base || address - base > MAXDWORD) return {0, 0, ScrollerLocationState::Unmapped};
    return {base, static_cast<DWORD>(address - base), ScrollerLocationState::Image};
}

ScrollerLocation readTable(ULONG_PTR object)
{
    if (!object) return {0, 0, ScrollerLocationState::Null};
    ULONG_PTR table{};
    if (!readMemory(object, 0, table)) return {0, 0, ScrollerLocationState::ReadFailed};
    return imageLocation(table, false);
}

bool readImageSlot(const ScrollerLocation& table, ULONG_PTR offset, ULONG_PTR& target)
{
    ULONG_PTR address{}, slot{};
    MEMORY_BASIC_INFORMATION region{};
    return table.state == ScrollerLocationState::Image &&
        addOffset(table.imageBase, table.rva, address) && addOffset(address, offset, slot) &&
        VirtualQuery(reinterpret_cast<LPCVOID>(slot), &region, sizeof(region)) &&
        region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.Protect == PAGE_READONLY &&
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) == table.imageBase &&
        containsBytes(region, slot, sizeof(target)) &&
        guardedCopy(&target, reinterpret_cast<LPCVOID>(slot), sizeof(target));
}

struct StackRange {
    ULONG_PTR low{}, high{};

    template <typename T>
    bool read(ULONG_PTR address, T& value) const
    {
        MEMORY_BASIC_INFORMATION region{};
        if (address < low || address >= high || address % alignof(T) || sizeof(T) > high - address ||
            !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || !readableMemory(region.Protect) ||
            !containsBytes(region, address, sizeof(T))) return false;
        return guardedCopy(&value, reinterpret_cast<LPCVOID>(address), sizeof(T));
    }

    bool caller(ULONG_PTR rsp, ULONG_PTR offset, ULONG_PTR& value) const
    {
        ULONG_PTR address{};
        return rsp >= low && addOffset(rsp, offset, address) && read(address, value);
    }
};

Observation inspectScroller(ULONG_PTR scroller, ULONG_PTR wbase, ULONG_PTR& window,
                            ULONG_PTR& tableAddress, ScrollerIdentity& identity)
{
    if (!scroller) return Observation::Mismatch;
    if (!readMemory(scroller, 0, tableAddress)) return Observation::ReadFailure;
    if (tableAddress < wbase) return Observation::Mismatch;
    const auto drawRva = scrollerDrawRva(tableAddress - wbase);
    if (!drawRva) return Observation::Mismatch;
    identity.scroller = imageLocation(tableAddress, false);
    if (identity.scroller.state != ScrollerLocationState::Image ||
        identity.scroller.imageBase != wbase) return Observation::ReadFailure;
    ULONG_PTR entry{}, dispatch{};
    if (!readImageSlot(identity.scroller, 0x58, entry) ||
        !readImageSlot(identity.scroller, 0xC0, dispatch)) return Observation::ReadFailure;
    if (entry != wbase + scrollerEntryRva || dispatch != wbase + drawRva)
        return Observation::Mismatch;
    identity.dispatchMethodRva = drawRva;

    if (!readMemory(scroller, 0x20, window)) return Observation::ReadFailure;
    if (!window) return Observation::Mismatch;
    ULONG_PTR reverse{};
    if (!readMemory(window, 0xB8, reverse)) return Observation::ReadFailure;
    return reverse == scroller ? Observation::Accepted : Observation::Mismatch;
}

Observation captureIdentity(const CONTEXT& ctx, ULONG_PTR wbase, ScrollerSite site,
                            ScrollerIdentity& identity)
{
    identity = {};
    if (!wbase || wbase > (std::numeric_limits<ULONG_PTR>::max)() - scrollerTables.back().tableRva - 0xC0)
        return Observation::ReadFailure;
    const bool dispatch = site == ScrollerSite::Dispatch;
    const auto scroller = dispatch ? ctx.Rsi : ctx.R14;
    const auto rect = dispatch ? ctx.Rbp : ctx.Rsi;

    ULONG_PTR window{}, table{};
    const auto ownerStatus = inspectScroller(scroller, wbase, window, table, identity);
    if (ownerStatus != Observation::Accepted) return ownerStatus;
    if (ctx.R8 != rect) return Observation::Mismatch;
    if (dispatch) {
        if (ctx.Rax != table || ctx.R9 != rect) return Observation::Mismatch;
    } else {
        ULONG_PTR config{}, expectedRect{}, slot{};
        if (ctx.Rdi < 0x40 || ctx.Rdi > 0x70 || ctx.Rdi % 0x10 ||
            !addOffset(scroller, 0x28, config) || ctx.R9 != config ||
            !addOffset(ctx.R15, ctx.Rdi, expectedRect) || rect != expectedRect ||
            !addOffset(ctx.R15, 0x20 + (ctx.Rdi - 0x40) / 2, slot) || ctx.Rbx != slot ||
            !ctx.Rcx) return Observation::Mismatch;
        ULONG_PTR component{};
        if (!readMemory(slot, 0, component)) return Observation::ReadFailure;
        if (ctx.Rcx != component) return Observation::Mismatch;
        identity.componentOffset = static_cast<DWORD>(ctx.Rdi);
        // This is the byte forwarded to the painter, not a verified print flag.
        identity.drawFlag = static_cast<BYTE>(ctx.R12);
        identity.component = readTable(component);
        if (identity.component.state == ScrollerLocationState::ReadFailed)
            return Observation::ReadFailure;
        identity.componentMethod.state = ScrollerLocationState::Unmapped;
        if (identity.component.state == ScrollerLocationState::Image) {
            ULONG_PTR method{};
            if (!readImageSlot(identity.component, 0x10, method)) return Observation::ReadFailure;
            identity.componentMethod = imageLocation(method, true);
        }
    }

    StackRange stack;
    GetCurrentThreadStackLimits(&stack.low, &stack.high);
    ULONG_PTR caller{};
    if (!stack.caller(ctx.Rsp, dispatch ? 0x108 : 0x88, caller) ||
        !readMemory(rect, 0, identity.rect)) return Observation::ReadFailure;
    identity.site = site;
    identity.window = readTable(window);
    if (identity.window.state == ScrollerLocationState::ReadFailed) return Observation::ReadFailure;
    // In particular, shared component paint can be called through draw wrappers.
    identity.caller = imageLocation(caller, true);
    return Observation::Accepted;
}

void recordCall(const CONTEXT& context, ULONG_PTR wbase, ScrollerSite site)
{
    const auto index = static_cast<size_t>(site);
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& count = counts[index];
    ScrollerIdentity identity{};
    switch (captureIdentity(context, wbase, site, identity)) {
    case Observation::ReadFailure: ++count.readFailures; break;
    case Observation::Mismatch: ++count.mismatches; break;
    case Observation::Accepted: {
        size_t i{};
        while (i < sampleCount && !(samples[i].identity == identity)) ++i;
        if (i == sampleCount && sampleCount == samples.size()) {
            ++count.capacityDrops;
            break;
        }
        if (i == sampleCount) samples[sampleCount++] = {identity, 1};
        else ++samples[i].calls;
        ++count.accepted;
        if (unresolvedScrollerIdentity(identity)) ++count.unknownIdentities;
        break;
    }
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

void countCall(const CONTEXT& context, ULONG_PTR wbase, ScrollerSite site)
{
    const auto until = deadline.load();
    if (!until || GetTickCount64() >= until) return;
    activeCollectors.fetch_add(1);
    if (deadline.load() && GetTickCount64() < until) recordCall(context, wbase, site);
    activeCollectors.fetch_sub(1);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto event = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
    for (size_t i = 0; event && i < scrollerSites.size(); ++i) {
        if (!replaySites[i].load(std::memory_order_acquire) ||
            handlerAddresses[i].load(std::memory_order_relaxed) != event) continue;
        const auto lastError = GetLastError();
        const auto site = static_cast<ScrollerSite>(i);
        auto& context = *exception->ContextRecord;
        context.Rdx = site == ScrollerSite::Dispatch ? context.R14 : context.Rbp;
        context.Rip = event + 3;
        countCall(context, event - scrollerSites[i].rva, site);
        SetLastError(lastError);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct PatchSite {
    BYTE* address{};
    DWORD originalProtection{};
    DWORD restoreProtection{};
    bool ownsBreakpoint{};
    bool needsFlush{};
};

using PatchSites = std::array<PatchSite, scrollerSites.size()>;

DWORD lastErrorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

bool signatureMatches(const BYTE* address, size_t index)
{
    std::array<BYTE, scrollerSites[0].signature.size()> bytes{};
    return guardedCopy(bytes.data(), address, bytes.size()) && bytes == scrollerSites[index].signature;
}

DWORD checkImageSite(PatchSite& site, HMODULE wbase, size_t index, DWORD pageSize)
{
    ULONG_PTR address{};
    if (!addOffset(reinterpret_cast<ULONG_PTR>(wbase), scrollerSites[index].rva, address))
        return ERROR_INVALID_ADDRESS;
    MEMORY_BASIC_INFORMATION region{};
    const auto size = scrollerSites[index].signature.size();
    if (!pageSize || size > pageSize - address % pageSize ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.AllocationBase != wbase || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        region.Protect != PAGE_EXECUTE_READ || !containsBytes(region, address, size))
        return ERROR_INVALID_ADDRESS;
    site.address = reinterpret_cast<BYTE*>(address);
    site.originalProtection = region.Protect;
    return signatureMatches(site.address, index) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

struct ModuleRef {
    HMODULE module{};
    ~ModuleRef() { if (module) FreeLibrary(module); }
    bool acquire(const wchar_t* name) { return GetModuleHandleExW(0, name, &module) != FALSE; }
};

DWORD prepare(PatchSites& sites)
{
    ModuleRef wbase, frame, graphics;
    if (!wbase.acquire(L"wbase83.dll") || !frame.acquire(L"frame.dll") || !graphics.acquire(L"grphcs.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.module), frameHash) ||
        !matchesSha256(modulePath(graphics.module), graphicsHash) ||
        !matchesSha256(modulePath(wbase.module), scrollerWbaseHash)) return ERROR_REVISION_MISMATCH;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    for (size_t i = 0; i < sites.size(); ++i)
        if (const auto error = checkImageSite(sites[i], wbase.module, i, system.dwPageSize)) return error;
    if (reinterpret_cast<ULONG_PTR>(sites[0].address) / system.dwPageSize ==
        reinterpret_cast<ULONG_PTR>(sites[1].address) / system.dwPageSize) return ERROR_INVALID_ADDRESS;
    if (IsDebuggerPresent()) return ERROR_BUSY;

    // Both instructions and the replay-only handler stay live for queued traps.
    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(wbase.module), &pinned) ||
        pinned != wbase.module ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return lastErrorOr(ERROR_DLL_INIT_FAILED);
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) return ERROR_DLL_INIT_FAILED;
    for (size_t i = 0; i < sites.size(); ++i)
        handlerAddresses[i].store(reinterpret_cast<ULONG_PTR>(sites[i].address), std::memory_order_relaxed);
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

DWORD install(PatchSite& site, size_t index)
{
    if (const auto error = makeWritable(site)) return error;
    DWORD error = ERROR_INVALID_DATA;
    if (site.restoreProtection == site.originalProtection && signatureMatches(site.address, index)) {
        auto* opcode = reinterpret_cast<volatile char*>(site.address);
        const auto originalByte = static_cast<char>(scrollerSites[index].signature.front());
        // Announce just before the CAS so a thread can replay the first owned trap.
        replaySites[index].store(true, std::memory_order_release);
        if (_InterlockedCompareExchange8(opcode, breakpoint, originalByte) == originalByte) {
            site.ownsBreakpoint = true;
            site.needsFlush = true;
            error = ERROR_SUCCESS;
        } else {
            replaySites[index].store(false, std::memory_order_release);
        }
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD installAll(PatchSites& sites)
{
    for (size_t i = 0; i < sites.size(); ++i)
        if (const auto error = install(sites[i], i)) return error;
    return ERROR_SUCCESS;
}

DWORD rollback(PatchSite& site, size_t index)
{
    DWORD error{};
    if (site.ownsBreakpoint) {
        if (const auto writableError = makeWritable(site)) return writableError;
        auto* opcode = reinterpret_cast<volatile char*>(site.address);
        const auto originalByte = static_cast<char>(scrollerSites[index].signature.front());
        if (_InterlockedCompareExchange8(opcode, originalByte, breakpoint) != breakpoint)
            error = ERROR_INVALID_DATA;
        site.ownsBreakpoint = false;
        site.needsFlush = true;
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD finishPatch(PatchSite& site, ScrollerPatchReport& report, size_t index)
{
    const auto error = rollback(site, index);
    if (site.ownsBreakpoint || site.needsFlush || site.restoreProtection) rollback(site, index);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(site.address, &region, sizeof(region)) && region.State == MEM_COMMIT)
        report.finalProtection = region.Protect;
    if (error) return error;
    if (report.finalProtection != site.originalProtection || !signatureMatches(site.address, index))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD finishAll(PatchSites& sites, ScrollerProbeTrace& trace)
{
    DWORD cleanup{};
    for (size_t i = sites.size(); i != 0; --i) {
        const auto index = i - 1;
        auto& report = trace.patches[index];
        report.cleanupError = finishPatch(sites[index], report, index);
        if (!cleanup) cleanup = report.cleanupError;
    }
    return cleanup;
}

DWORD copySamples(ScrollerProbeTrace& trace)
{
    const auto giveUp = GetTickCount64() + drainMs;
    while (activeCollectors.load() || !TryAcquireSRWLockExclusive(&sampleLock)) {
        if (GetTickCount64() >= giveUp) return ERROR_TIMEOUT;
        Sleep(1);
    }
    trace.sampleCount = sampleCount;
    trace.samples = samples;
    trace.counts = counts;
    for (size_t i = 0; i < counts.size(); ++i)
        trace.counts[i].busyDrops = busyDrops[i].load(std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&sampleLock);
    return ERROR_SUCCESS;
}

DWORD runProbe(ScrollerProbeTrace& trace)
{
    if (attempted) return trace.patchError = ERROR_ALREADY_EXISTS;
    attempted = true;
    if (IsDebuggerPresent()) return trace.patchError = ERROR_BUSY;
    PatchSites sites;
    try {
        trace.patchError = prepare(sites);
    } catch (const std::bad_alloc&) {
        trace.patchError = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (trace.patchError) return trace.patchError;

    for (size_t i = 0; i < sites.size(); ++i)
        trace.patches[i].originalProtection = sites[i].originalProtection;
    trace.patchError = IsDebuggerPresent() ? ERROR_BUSY : installAll(sites);
    // Capture starts only after both breakpoints have been installed and both
    // pages have recovered their original protection.
    if (!trace.patchError) {
        const auto start = GetTickCount64();
        const auto end = start + captureMs;
        deadline.store(end);
        ULONGLONG now{};
        while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
        deadline.store(0);
        trace.elapsedMs = static_cast<DWORD>((std::min)(now - start, ULONGLONG{MAXDWORD}));
    }
    const auto cleanup = finishAll(sites, trace);
    if (!trace.patchError) trace.snapshotError = copySamples(trace);
    return cleanup ? cleanup : (trace.patchError ? trace.patchError : trace.snapshotError);
}

bool validInput(const void* output)
{
    if (!output || reinterpret_cast<ULONG_PTR>(output) % alignof(ScrollerProbeTrace)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(ScrollerProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
    return header[0] == sizeof(ScrollerProbeTrace) && header[1] == scrollerProbeVersion;
}

DWORD publish(void* output, const ScrollerProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (readFaultFilter(GetExceptionCode())) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI ScrollerIdentityProbeAbiV1(void*)
{
    return scrollerProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunScrollerIdentityProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    ScrollerProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
