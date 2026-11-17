#include "local_frame_probe.hpp"
#include "list_identity.hpp"

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
std::array<std::atomic<ULONG_PTR>, frameSites.size()> handlerAddresses{};
std::array<std::atomic<bool>, frameSites.size()> replaySites{};
std::atomic<ULONGLONG> deadline{};
std::array<std::atomic<ULONGLONG>, frameSites.size()> busyDrops{};
std::atomic<DWORD> activeCollectors{};
bool attempted{};
DWORD sampleCount{};
std::array<FrameProbeCounts, frameSites.size()> counts{};
std::array<FrameProbeSample, frameProbeCapacity> samples{};

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

constexpr ULONG_PTR barTableRva = 0xB23F20;
constexpr ULONG_PTR barSideRva = 0xB23848;

enum class Observation { Accepted, Mismatch, ReadFailure, Printing };

FrameLocation imageLocation(ULONG_PTR address, bool code)
{
    if (!address) return {0, 0, FrameLocationState::Null};
    MEMORY_BASIC_INFORMATION region{};
    const SIZE_T length = code ? 1 : sizeof(ULONG_PTR);
    if ((!code && address % alignof(ULONG_PTR)) ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (code ? region.Protect != PAGE_EXECUTE_READ && region.Protect != PAGE_EXECUTE :
                region.Protect != PAGE_READONLY) || !containsBytes(region, address, length))
        return {0, 0, FrameLocationState::Unmapped};
    const auto base = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    if (!base || address < base || address - base > MAXDWORD) return {0, 0, FrameLocationState::Unmapped};
    return {base, static_cast<DWORD>(address - base), FrameLocationState::Image};
}

FrameLocation readTable(ULONG_PTR object)
{
    if (!object) return {0, 0, FrameLocationState::Null};
    ULONG_PTR table{};
    if (!readPointer(object, 0, table)) return {0, 0, FrameLocationState::ReadFailed};
    return imageLocation(table, false);
}

Observation verifyDrawMethod(const FrameLocation& table, ULONG_PTR target, FrameLocation& method)
{
    method.state = FrameLocationState::Unmapped;
    if (table.state != FrameLocationState::Image) return Observation::Accepted;
    ULONG_PTR address{}, slot{};
    if (!addOffset(table.imageBase, table.rva, address) || !addOffset(address, 0x18, slot))
        return Observation::ReadFailure;
    ULONG_PTR current{};
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(slot), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.Protect != PAGE_READONLY ||
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) != table.imageBase ||
        !containsBytes(region, slot, sizeof(current)) ||
        !guardedCopy(&current, reinterpret_cast<LPCVOID>(slot), sizeof(current)))
        return Observation::ReadFailure;
    if (current != target) return Observation::Mismatch;
    method = imageLocation(target, true);
    return Observation::Accepted;
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

Observation inspectBar(ULONG_PTR bar, ULONG_PTR frame, ULONG_PTR& owner)
{
    if (!bar) return Observation::Mismatch;
    ULONG_PTR table{};
    if (!readPointer(bar, 0, table)) return Observation::ReadFailure;
    if (table != frame + barTableRva) return Observation::Mismatch;
    if (!readPointer(bar, 0x58, table)) return Observation::ReadFailure;
    if (table != frame + barSideRva) return Observation::Mismatch;
    if (!readPointer(bar, 0x678, owner)) return Observation::ReadFailure;
    if (!owner) return Observation::Mismatch;
    ULONG_PTR reverse{};
    if (!readPointer(owner, 0xD0, reverse)) return Observation::ReadFailure;
    return reverse == bar ? Observation::Accepted : Observation::Mismatch;
}

Observation captureIdentity(const CONTEXT& ctx, ULONG_PTR frame, FrameSite site, FrameIdentity& identity)
{
    if (!frame || frame > (std::numeric_limits<ULONG_PTR>::max)() - barTableRva)
        return Observation::ReadFailure;
    StackRange stack;
    GetCurrentThreadStackLimits(&stack.low, &stack.high);
    const bool background = site == FrameSite::Background;
    ULONG_PTR caller{};
    if (!stack.caller(ctx.Rsp, background ? 0x98 : 0xA8, caller)) return Observation::ReadFailure;
    if (caller != frame + (background ? 0x70FB7B : 0x70FC9C)) return Observation::Mismatch;
    if (!background) {
        if (!stack.caller(ctx.Rsp, 0x1A8, caller)) return Observation::ReadFailure;
        if (caller != frame + 0x70FBA5) return Observation::Mismatch;
    }

    ULONG_PTR owner{};
    const auto barStatus = inspectBar(background ? ctx.Rbx : ctx.Rdi, frame, owner);
    if (barStatus != Observation::Accepted) return barStatus;
    if (background && static_cast<BYTE>(ctx.Rdi)) return Observation::Printing;

    ULONG_PTR rect{};
    if (background) {
        if (!addOffset(ctx.Rsp, 0xF0, rect)) return Observation::ReadFailure;
        if (ctx.R14 != rect) return Observation::Mismatch;
    } else {
        ULONG_PTR expectedFrame{};
        if (ctx.Rbp < 0x20 || ctx.Rbp > stack.high || !addOffset(ctx.Rsp, 0x80, expectedFrame))
            return Observation::ReadFailure;
        rect = ctx.Rbp - 0x20;
        if (ctx.Rbp != expectedFrame || ctx.R8 != rect || ctx.R9 > MAXBYTE) return Observation::Mismatch;
    }
    if (!stack.read(rect, identity.rect)) return Observation::ReadFailure;
    identity.site = site;
    identity.owner = readTable(owner);
    if (!background) {
        identity.rawItem = readTable(ctx.Rbx);
        identity.drawInterface = readTable(ctx.Rcx);
    }
    if (identity.owner.state == FrameLocationState::ReadFailed ||
        identity.rawItem.state == FrameLocationState::ReadFailed ||
        identity.drawInterface.state == FrameLocationState::ReadFailed) return Observation::ReadFailure;
    if (!background) {
        const auto drawStatus = verifyDrawMethod(identity.drawInterface, ctx.R10, identity.drawMethod);
        if (drawStatus != Observation::Accepted) return drawStatus;
    }
    identity.colorOrFlag = background ? static_cast<DWORD>(ctx.R8) : static_cast<DWORD>(ctx.R9);
    return Observation::Accepted;
}

void recordCall(const CONTEXT& context, ULONG_PTR frame, FrameSite site)
{
    const auto index = static_cast<size_t>(site);
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& count = counts[index];
    FrameIdentity identity{};
    switch (captureIdentity(context, frame, site, identity)) {
    case Observation::ReadFailure: ++count.readFailures; break;
    case Observation::Mismatch: ++count.mismatches; break;
    case Observation::Printing: ++count.printing; break;
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
        if (identity.owner.state != FrameLocationState::Image ||
            (site == FrameSite::Child && (identity.rawItem.state != FrameLocationState::Image ||
             identity.drawInterface.state != FrameLocationState::Image ||
             identity.drawMethod.state != FrameLocationState::Image))) ++count.unknownIdentities;
        break;
    }
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

void countCall(const CONTEXT& context, ULONG_PTR frame, FrameSite site)
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
    for (size_t i = 0; event && i < frameSites.size(); ++i) {
        if (!replaySites[i].load(std::memory_order_acquire) ||
            handlerAddresses[i].load(std::memory_order_relaxed) != event) continue;
        const auto lastError = GetLastError();
        const auto site = static_cast<FrameSite>(i);
        auto& context = *exception->ContextRecord;
        context.Rdx = site == FrameSite::Background ? context.R14 : context.Rsi;
        context.Rip = event + 3;
        countCall(context, event - frameSites[i].rva, site);
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

using PatchSites = std::array<PatchSite, frameSites.size()>;

DWORD lastErrorOr(DWORD fallback)
{
    const auto error = GetLastError();
    return error ? error : fallback;
}

bool signatureMatches(const BYTE* address, size_t index)
{
    std::array<BYTE, frameSites[0].signature.size()> bytes{};
    return guardedCopy(bytes.data(), address, bytes.size()) && bytes == frameSites[index].signature;
}

DWORD checkImageSite(PatchSite& site, HMODULE frame, size_t index, DWORD pageSize)
{
    ULONG_PTR address{};
    if (!addOffset(reinterpret_cast<ULONG_PTR>(frame), frameSites[index].rva, address))
        return ERROR_INVALID_ADDRESS;
    MEMORY_BASIC_INFORMATION region{};
    const auto size = frameSites[index].signature.size();
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
    ModuleRef frame, graphics, frontend;
    if (!frame.acquire(L"frame.dll") || !graphics.acquire(L"grphcs.dll") || !frontend.acquire(L"frntend.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.module), frameHash) ||
        !matchesSha256(modulePath(graphics.module), graphicsHash) ||
        !matchesSha256(modulePath(frontend.module), listFrontendHash)) return ERROR_REVISION_MISMATCH;
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
        const auto originalByte = static_cast<char>(frameSites[index].signature.front());
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
        const auto originalByte = static_cast<char>(frameSites[index].signature.front());
        if (_InterlockedCompareExchange8(opcode, originalByte, breakpoint) != breakpoint)
            error = ERROR_INVALID_DATA;
        site.ownsBreakpoint = false;
        site.needsFlush = true;
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD finishPatch(PatchSite& site, FramePatchReport& report, size_t index)
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

DWORD finishAll(PatchSites& sites, FrameProbeTrace& trace)
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

DWORD copySamples(FrameProbeTrace& trace)
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

DWORD runProbe(FrameProbeTrace& trace)
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
    if (!output || reinterpret_cast<ULONG_PTR>(output) % alignof(FrameProbeTrace)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(FrameProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
    return header[0] == sizeof(FrameProbeTrace) && header[1] == frameProbeVersion;
}

DWORD publish(void* output, const FrameProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (readFaultFilter(GetExceptionCode())) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI FrameProbeAbiV1(void*)
{
    return frameProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunLocalFrameProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    FrameProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
