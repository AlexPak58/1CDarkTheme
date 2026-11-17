#include "dock_fill_probe.hpp"

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
std::array<std::atomic<ULONG_PTR>, dockSites.size()> handlerAddresses{};
std::array<std::atomic<bool>, dockSites.size()> replaySites{};
std::atomic<ULONGLONG> deadline{};
std::array<std::atomic<ULONGLONG>, dockSites.size()> busyDrops{};
std::atomic<DWORD> activeCollectors{};
bool attempted{};
DWORD sampleCount{};
std::array<DockProbeCounts, dockSites.size()> counts{};
std::array<DockProbeSample, dockProbeCapacity> samples{};

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

DockLocation callerLocation(ULONG_PTR address)
{
    if (!address) return {0, 0, DockLocationState::Null};
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (region.Protect != PAGE_EXECUTE_READ && region.Protect != PAGE_EXECUTE) ||
        !containsBytes(region, address, 1)) return {0, 0, DockLocationState::Unmapped};
    const auto base = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    if (!base || address < base || address - base > MAXDWORD) return {0, 0, DockLocationState::Unmapped};
    return {base, static_cast<DWORD>(address - base), DockLocationState::Image};
}

bool imageTableSlots(ULONG_PTR frame, DWORD tableRva, SIZE_T length)
{
    ULONG_PTR table{};
    MEMORY_BASIC_INFORMATION region{};
    return addOffset(frame, tableRva, table) &&
        VirtualQuery(reinterpret_cast<LPCVOID>(table), &region, sizeof(region)) &&
        region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.Protect == PAGE_READONLY &&
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) == frame &&
        containsBytes(region, table, length);
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

Observation inspectDock(ULONG_PTR root, ULONG_PTR frame, DockSite site, DockIdentity& identity)
{
    ULONG_PTR rootTable{}, sideTable{}, drawTable{};
    if (!readMemory(root, 0, rootTable)) return Observation::ReadFailure;
    if (rootTable < frame) return Observation::Mismatch;
    const auto* owner = dockOwner(site, rootTable - frame);
    if (!owner) return Observation::Mismatch;
    // 9123BC / 923D9B / 9124FC set root, +B0 and +188 together.
    if (!readMemory(root, 0xB0, sideTable) || !readMemory(root, 0x188, drawTable))
        return Observation::ReadFailure;
    ULONG_PTR expectedSide{}, expectedDraw{}, expectedMethod{};
    const auto& definition = dockSites[static_cast<size_t>(site)];
    if (!addOffset(frame, owner->sideRva, expectedSide) ||
        !addOffset(frame, owner->drawRva, expectedDraw) ||
        !addOffset(frame, definition.drawMethodRva, expectedMethod)) return Observation::ReadFailure;
    if (sideTable != expectedSide || drawTable != expectedDraw) return Observation::Mismatch;
    if (!imageTableSlots(frame, owner->rootRva, sizeof(ULONG_PTR)) ||
        !imageTableSlots(frame, owner->sideRva, sizeof(ULONG_PTR)) ||
        !imageTableSlots(frame, owner->drawRva, 0x18 + sizeof(ULONG_PTR))) return Observation::ReadFailure;
    ULONG_PTR method{};
    if (!readMemory(drawTable, 0x18, method)) return Observation::ReadFailure;
    if (method != expectedMethod) return Observation::Mismatch;
    identity.rootTypeRva = owner->rootRva;
    return Observation::Accepted;
}

Observation captureIdentity(const CONTEXT& ctx, ULONG_PTR frame, DockSite site, DockIdentity& identity)
{
    identity = {};
    if (!frame) return Observation::ReadFailure;
    const bool basic = site == DockSite::Basic;
    const auto drawInterface = basic ? ctx.Rbx : ctx.R14;
    if (drawInterface <= 0x188) return Observation::Mismatch;
    const auto root = drawInterface - 0x188;
    const auto ownerStatus = inspectDock(root, frame, site, identity);
    if (ownerStatus != Observation::Accepted) return ownerStatus;

    ULONG_PTR brush{};
    if (!readMemory(root, 0x270, brush)) return Observation::ReadFailure;
    if (brush != ctx.R8) return Observation::Mismatch;

    if (!readMemory(root, 0x208, identity.full) || !readMemory(root, 0x268, identity.rawColor))
        return Observation::ReadFailure;
    // The constructor at 9124A4 initializes 0x10. The edge switch at
    // 9321B5 distinguishes 1/2/4/8, so preserve the raw field.
    if (!basic && !readMemory(root, 0x288, identity.orientationRaw)) return Observation::ReadFailure;

    StackRange stack;
    GetCurrentThreadStackLimits(&stack.low, &stack.high);
    ULONG_PTR caller{}, clippedAddress{};
    if (!addOffset(ctx.Rsp, 0x40, clippedAddress)) return Observation::ReadFailure;
    // In 9320E0 the frame register is established before sub rsp,80h.
    if (!basic) {
        ULONG_PTR expectedFrame{};
        if (!addOffset(ctx.Rsp, 0x80, expectedFrame)) return Observation::ReadFailure;
        if (ctx.Rbp != expectedFrame || ctx.Rdx != ctx.R15) return Observation::Mismatch;
        // RBP - 0x40 is equivalent to RSP + 0x40 after the relation check.
    }
    if (!stack.caller(ctx.Rsp, basic ? 0x88 : 0xA8, caller) ||
        !stack.read(clippedAddress, identity.clipped)) return Observation::ReadFailure;
    if (!isClippedDockRect(identity.full, identity.clipped)) return Observation::Mismatch;
    identity.site = site;
    identity.caller = callerLocation(caller);
    return Observation::Accepted;
}

void recordCall(const CONTEXT& context, ULONG_PTR frame, DockSite site)
{
    const auto index = static_cast<size_t>(site);
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& count = counts[index];
    DockIdentity identity{};
    switch (captureIdentity(context, frame, site, identity)) {
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
        if (identity.caller.state != DockLocationState::Image) ++count.unknownIdentities;
        break;
    }
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

void countCall(const CONTEXT& context, ULONG_PTR frame, DockSite site)
{
    const auto until = deadline.load();
    if (!until || GetTickCount64() >= until) return;
    activeCollectors.fetch_add(1);
    if (deadline.load() && GetTickCount64() < until) recordCall(context, frame, site);
    activeCollectors.fetch_sub(1);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto event = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
    for (size_t i = 0; event && i < dockSites.size(); ++i) {
        if (!replaySites[i].load(std::memory_order_acquire) ||
            handlerAddresses[i].load(std::memory_order_relaxed) != event) continue;
        const auto lastError = GetLastError();
        const auto site = static_cast<DockSite>(i);
        auto& context = *exception->ContextRecord;
        if (site == DockSite::Basic) context.Rdx = context.Rbp;
        else context.Rcx = context.Rdi;
        context.Rip = event + 3;
        countCall(context, event - dockSites[i].rva, site);
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

using PatchSites = std::array<PatchSite, dockSites.size()>;

DWORD lastErrorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

bool signatureMatches(const BYTE* address, size_t index)
{
    std::array<BYTE, dockSites[0].signature.size()> bytes{};
    return guardedCopy(bytes.data(), address, bytes.size()) && bytes == dockSites[index].signature;
}

DWORD checkImageSite(PatchSite& site, HMODULE frame, size_t index, DWORD pageSize)
{
    ULONG_PTR address{};
    if (!addOffset(reinterpret_cast<ULONG_PTR>(frame), dockSites[index].rva, address))
        return ERROR_INVALID_ADDRESS;
    MEMORY_BASIC_INFORMATION region{};
    const auto size = dockSites[index].signature.size();
    if (!pageSize || size > pageSize - address % pageSize ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.AllocationBase != frame || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
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
    ModuleRef frame, graphics;
    if (!frame.acquire(L"frame.dll") || !graphics.acquire(L"grphcs.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.module), frameHash) ||
        !matchesSha256(modulePath(graphics.module), graphicsHash)) return ERROR_REVISION_MISMATCH;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    for (size_t i = 0; i < sites.size(); ++i)
        if (const auto error = checkImageSite(sites[i], frame.module, i, system.dwPageSize)) return error;
    if (reinterpret_cast<ULONG_PTR>(sites[0].address) / system.dwPageSize ==
        reinterpret_cast<ULONG_PTR>(sites[1].address) / system.dwPageSize) return ERROR_INVALID_ADDRESS;
    if (IsDebuggerPresent()) return ERROR_BUSY;

    // Both instructions and the replay-only handler stay live for queued traps.
    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frame.module), &pinned) ||
        pinned != frame.module ||
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
        const auto originalByte = static_cast<char>(dockSites[index].signature.front());
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
        const auto originalByte = static_cast<char>(dockSites[index].signature.front());
        if (_InterlockedCompareExchange8(opcode, originalByte, breakpoint) != breakpoint)
            error = ERROR_INVALID_DATA;
        site.ownsBreakpoint = false;
        site.needsFlush = true;
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD finishPatch(PatchSite& site, DockPatchReport& report, size_t index)
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

DWORD finishAll(PatchSites& sites, DockProbeTrace& trace)
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

DWORD copySamples(DockProbeTrace& trace)
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

DWORD runProbe(DockProbeTrace& trace)
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
    if (!output || reinterpret_cast<ULONG_PTR>(output) % alignof(DockProbeTrace)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(DockProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
    return header[0] == sizeof(DockProbeTrace) && header[1] == dockProbeVersion;
}

DWORD publish(void* output, const DockProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (readFaultFilter(GetExceptionCode())) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DockFillProbeAbiV1(void*)
{
    return dockProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunDockFillProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    DockProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
