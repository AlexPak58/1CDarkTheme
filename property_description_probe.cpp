#include "property_description_probe.hpp"

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <new>

namespace {

using namespace propertyDescription;
constexpr ULONGLONG captureMs = 15000;
constexpr ULONGLONG drainMs = 100;
constexpr char breakpoint = static_cast<char>(0xCC);
constexpr std::array<DWORD, 2> returnRvas{0x5D1545, 0x3E796C};

SRWLOCK runLock = SRWLOCK_INIT;
SRWLOCK sampleLock = SRWLOCK_INIT;
std::atomic<ULONG_PTR> handlerAddress{};
std::atomic<bool> replaySite{};
std::atomic<ULONGLONG> deadline{}, busyDrops{};
std::atomic<DWORD> activeCollectors{};
bool attempted{};
DWORD sampleCount{};
PropertyProbeCounts counts{};
std::array<PropertySample, propertyProbeCapacity> samples{};

static_assert(std::atomic<ULONG_PTR>::is_always_lock_free);
static_assert(std::atomic<ULONGLONG>::is_always_lock_free);
static_assert(std::atomic<DWORD>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

struct StackRange {
    ULONG_PTR low{}, high{};

    template <typename T>
    bool read(ULONG_PTR address, T& value) const
    {
        return address >= low && address < high && sizeof(T) <= high - address &&
            GuardedReader{}.read(address, 0, value);
    }

    template <typename T>
    bool readAt(ULONG_PTR rsp, ULONG_PTR offset, T& value) const
    {
        ULONG_PTR address{};
        return rsp >= low && addOffset(rsp, offset, address) && read(address, value);
    }
};

template <typename Reader = GuardedReader>
Outcome captureIdentity(const CONTEXT& ctx, ULONG_PTR frame, Identity& identity, PropertyRect& rect,
                        const Reader& read = {})
{
    if (!frame || !ctx.Rbp) return Outcome::ReadFailure;
    StackRange stack;
    GetCurrentThreadStackLimits(&stack.low, &stack.high);
    ULONG_PTR caller{}, parent{};
    if (!stack.readAt(ctx.Rsp, 0xA8, caller) || !stack.readAt(ctx.Rsp, 0x188, parent))
        return Outcome::ReadFailure;
    if (!matchesRva(frame, caller, returnRvas[0]) || !matchesRva(frame, parent, returnRvas[1]))
        return Outcome::Mismatch;
    // The type-13 branch passes the inset RECT instead of the ordinary paint RECT.
    if (!matchesOffset(ctx.Rsp, 0x270, ctx.R14) && !matchesOffset(ctx.Rsp, 0x120, ctx.R14))
        return Outcome::Mismatch;
    LONG width{}, height{};
    DWORD rop{};
    if (!stack.read(ctx.R14, rect) || !stack.readAt(ctx.Rsp, 0x20, width) ||
        !stack.readAt(ctx.Rsp, 0x28, height) || !stack.readAt(ctx.Rsp, 0x30, rop))
        return Outcome::ReadFailure;
    if (rop != 0x00F00021 || !validPropertyRect(rect) ||
        static_cast<LONG>(ctx.R8) != rect.left || static_cast<LONG>(ctx.R9) != rect.top ||
        static_cast<LONGLONG>(rect.right) - rect.left != width ||
        static_cast<LONGLONG>(rect.bottom) - rect.top != height) return Outcome::Mismatch;
    return inspect(frame, ctx.Rdi, identity, read);
}

void recordSample(const Identity& identity, const PropertyRect& rect)
{
    size_t index{};
    while (index < sampleCount &&
        !(samples[index].identity == identity && samples[index].rect == rect)) ++index;
    if (index == sampleCount && sampleCount == samples.size()) {
        ++counts.capacityDrops;
        return;
    }
    if (index == sampleCount) samples[sampleCount++] = {identity, rect, 1};
    else ++samples[index].calls;
    switch (identity.kind) {
    case Kind::Description: ++counts.description; break;
    case Kind::List: ++counts.list; break;
    case Kind::ForeignId10: ++counts.foreignId10; break;
    }
}

void recordCall(const CONTEXT& context, ULONG_PTR frame)
{
    if (!TryAcquireSRWLockExclusive(&sampleLock)) {
        busyDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Identity identity{};
    PropertyRect rect{};
    switch (captureIdentity(context, frame, identity, rect)) {
    case Outcome::ReadFailure: ++counts.readFailures; break;
    case Outcome::Mismatch: ++counts.mismatches; break;
    case Outcome::Accepted: recordSample(identity, rect); break;
    }
    ReleaseSRWLockExclusive(&sampleLock);
}

void countCall(const CONTEXT& context, ULONG_PTR frame)
{
    const auto until = deadline.load();
    if (!until || GetTickCount64() >= until) return;
    activeCollectors.fetch_add(1);
    if (deadline.load() && GetTickCount64() < until) recordCall(context, frame);
    activeCollectors.fetch_sub(1);
}

LONG CALLBACK onBreakpoint(EXCEPTION_POINTERS* exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto event = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
    if (!event || !replaySite.load(std::memory_order_acquire) ||
        handlerAddress.load(std::memory_order_relaxed) != event) return EXCEPTION_CONTINUE_SEARCH;
    const auto lastError = GetLastError();
    auto& context = *exception->ContextRecord;
    context.Rdx = context.Rbp;
    context.Rip = event + 3;
    countCall(context, event - propertySiteRva);
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
    std::array<BYTE, propertySiteSignature.size()> bytes{};
    return guardedCopy(bytes.data(), address, bytes.size()) && bytes == propertySiteSignature;
}

DWORD checkImageSite(PatchSite& site, HMODULE frame, DWORD pageSize)
{
    ULONG_PTR address{};
    if (!addOffset(reinterpret_cast<ULONG_PTR>(frame), propertySiteRva, address))
        return ERROR_INVALID_ADDRESS;
    MEMORY_BASIC_INFORMATION region{};
    const auto size = propertySiteSignature.size();
    if (!pageSize || size > pageSize - address % pageSize ||
        !VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.AllocationBase != frame || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        region.Protect != PAGE_EXECUTE_READ || !containsBytes(region, address, size))
        return ERROR_INVALID_ADDRESS;
    site.address = reinterpret_cast<BYTE*>(address);
    site.originalProtection = region.Protect;
    return signatureMatches(site.address) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

struct ModuleRef {
    HMODULE module{};
    ~ModuleRef() { if (module) FreeLibrary(module); }
    bool acquire(const wchar_t* name) { return GetModuleHandleExW(0, name, &module) != FALSE; }
};

DWORD prepare(PatchSite& site)
{
    ModuleRef frame, graphics;
    if (!frame.acquire(L"frame.dll") || !graphics.acquire(L"grphcs.dll"))
        return ERROR_MOD_NOT_FOUND;
    if (!matchesSha256(modulePath(nullptr), exeHash) ||
        !matchesSha256(modulePath(frame.module), frameHash) ||
        !matchesSha256(modulePath(graphics.module), graphicsHash)) return ERROR_REVISION_MISMATCH;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (const auto error = checkImageSite(site, frame.module, system.dwPageSize)) return error;
    if (IsDebuggerPresent()) return ERROR_BUSY;

    // The instruction and replay-only handler stay live for queued traps.
    HMODULE pinned{};
    constexpr auto pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(frame.module), &pinned) ||
        pinned != frame.module ||
        !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&onBreakpoint), &pinned))
        return lastErrorOr(ERROR_DLL_INIT_FAILED);
    if (!AddVectoredExceptionHandler(1, onBreakpoint)) return ERROR_DLL_INIT_FAILED;
    handlerAddress.store(reinterpret_cast<ULONG_PTR>(site.address), std::memory_order_relaxed);
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
    DWORD error = ERROR_INVALID_DATA;
    if (site.restoreProtection == site.originalProtection && signatureMatches(site.address)) {
        auto* opcode = reinterpret_cast<volatile char*>(site.address);
        const auto originalByte = static_cast<char>(propertySiteSignature.front());
        // Announce just before the CAS so a thread can replay the first owned trap.
        replaySite.store(true, std::memory_order_release);
        if (_InterlockedCompareExchange8(opcode, breakpoint, originalByte) == originalByte) {
            site.ownsBreakpoint = true;
            site.needsFlush = true;
            error = ERROR_SUCCESS;
        } else {
            replaySite.store(false, std::memory_order_release);
        }
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
        const auto originalByte = static_cast<char>(propertySiteSignature.front());
        if (_InterlockedCompareExchange8(opcode, originalByte, breakpoint) != breakpoint)
            error = ERROR_INVALID_DATA;
        site.ownsBreakpoint = false;
        site.needsFlush = true;
    }
    const auto finishError = flushAndProtect(site);
    return error ? error : finishError;
}

DWORD finishPatch(PatchSite& site, PropertyProbePatch& report)
{
    const auto error = rollback(site);
    if (site.ownsBreakpoint || site.needsFlush || site.restoreProtection) rollback(site);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(site.address, &region, sizeof(region)) && region.State == MEM_COMMIT)
        report.finalProtection = region.Protect;
    if (error) return error;
    if (report.finalProtection != site.originalProtection || !signatureMatches(site.address))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD copySamples(PropertyProbeTrace& trace)
{
    const auto giveUp = GetTickCount64() + drainMs;
    while (activeCollectors.load() || !TryAcquireSRWLockExclusive(&sampleLock)) {
        if (GetTickCount64() >= giveUp) return ERROR_TIMEOUT;
        Sleep(1);
    }
    trace.sampleCount = sampleCount;
    trace.samples = samples;
    trace.counts = counts;
    trace.counts.busyDrops = busyDrops.load(std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&sampleLock);
    return ERROR_SUCCESS;
}

DWORD runProbe(PropertyProbeTrace& trace)
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

    trace.patch.originalProtection = site.originalProtection;
    trace.patchError = IsDebuggerPresent() ? ERROR_BUSY : install(site);
    // Capture begins once the breakpoint is armed and the page protection restored.
    if (!trace.patchError) {
        const auto start = GetTickCount64();
        const auto end = start + captureMs;
        deadline.store(end);
        ULONGLONG now{};
        while ((now = GetTickCount64()) < end) Sleep(static_cast<DWORD>(end - now));
        deadline.store(0);
        trace.elapsedMs = static_cast<DWORD>((std::min)(now - start, ULONGLONG{MAXDWORD}));
    }
    const auto cleanup = trace.patch.cleanupError = finishPatch(site, trace.patch);
    if (!trace.patchError) trace.snapshotError = copySamples(trace);
    return cleanup ? cleanup : (trace.patchError ? trace.patchError : trace.snapshotError);
}

bool validInput(const void* output)
{
    if (!output || reinterpret_cast<ULONG_PTR>(output) % alignof(PropertyProbeTrace)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(output, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        !containsBytes(region, reinterpret_cast<ULONG_PTR>(output), sizeof(PropertyProbeTrace)) ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY &&
         region.Protect != PAGE_EXECUTE_READWRITE && region.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    DWORD header[2]{};
    __try {
        std::memcpy(header, output, sizeof(header));
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
    return header[0] == sizeof(PropertyProbeTrace) && header[1] == propertyProbeVersion;
}

DWORD publish(void* output, const PropertyProbeTrace& trace, DWORD result)
{
    __try {
        std::memcpy(output, &trace, sizeof(trace));
    } __except (readFaultFilter(GetExceptionCode())) {
        return result ? result : ERROR_NOACCESS;
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI PropertyDescriptionProbeAbiV1(void*)
{
    return propertyProbeVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI RunPropertyDescriptionProbeV1(void* output)
{
    if (!validInput(output)) return ERROR_INVALID_PARAMETER;
    PropertyProbeTrace trace;
    if (!TryAcquireSRWLockExclusive(&runLock)) return publish(output, trace, trace.patchError = ERROR_BUSY);
    const auto result = runProbe(trace);
    ReleaseSRWLockExclusive(&runLock);
    return publish(output, trace, result);
}
