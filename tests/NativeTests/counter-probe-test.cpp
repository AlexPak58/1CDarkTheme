#include "search_counter_probe.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
bool denyWrite{};
bool denyRestore{};
bool denyFlush{};
bool failNextFlush{};
ULONGLONG tick = 100;

BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous)
{
    if ((denyWrite && protection == PAGE_EXECUTE_READWRITE) || (denyRestore && protection == PAGE_EXECUTE_READ)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return VirtualProtect(address, size, protection, previous);
}

BOOL WINAPI fixtureFlush(HANDLE process, LPCVOID address, SIZE_T size)
{
    if (denyFlush || failNextFlush) {
        failNextFlush = false;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushInstructionCache(process, address, size);
}

ULONGLONG WINAPI fixtureTick() { return tick; }
}

#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#define GetTickCount64 fixtureTick
#include "search_counter_probe.cpp"
#undef GetTickCount64
#undef FlushInstructionCache
#undef VirtualProtect

namespace {

void check(bool ok, const char* details)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", details);
        std::exit(1);
    }
}

void protect(BYTE* page, DWORD protection)
{
    DWORD ignored{};
    check(VirtualProtect(page, 1, protection, &ignored) != FALSE, "set fixture protection");
}

void expectRestored(const PatchSite& site, const CounterProbeTrace& trace)
{
    check(!site.ownsBreakpoint && !site.needsFlush && !site.restoreProtection &&
          trace.finalProtection == PAGE_EXECUTE_READ &&
          std::memcmp(site.address, counterProbeSignature.data(), counterProbeSignature.size()) == 0,
          "restore complete signature and original protection");
}

void testRollback(BYTE* address)
{
    auto* page = address; // A protection change rounds this address down to its page.
    PatchSite site{address, PAGE_EXECUTE_READ};
    CounterProbeTrace trace;
    denyWrite = true;
    check(install(site) == ERROR_ACCESS_DENIED && !finishPatch(site, trace), "reject initial writable failure");
    denyWrite = false;
    expectRestored(site, trace);

    protect(page, PAGE_EXECUTE_READWRITE);
    address[2] = 0xC7;
    protect(page, PAGE_EXECUTE_READ);
    check(install(site) == ERROR_INVALID_DATA && address[0] == counterProbeSignature[0],
          "all six bytes are checked at patch time");
    protect(page, PAGE_EXECUTE_READWRITE);
    address[2] = counterProbeSignature[2];
    protect(page, PAGE_EXECUTE_READ);

    failNextFlush = true;
    check(install(site) == ERROR_WRITE_FAULT && site.ownsBreakpoint &&
          !finishPatch(site, trace), "rollback after initial flush failure");
    expectRestored(site, trace);

    denyRestore = true;
    check(install(site) == ERROR_ACCESS_DENIED && site.ownsBreakpoint && site.restoreProtection,
          "installation reports protection failure");
    check(finishPatch(site, trace) == ERROR_ACCESS_DENIED &&
          trace.finalProtection == PAGE_EXECUTE_READWRITE && address[0] == counterProbeSignature[0],
          "persistent protection failure is exposed after opcode restoration");
    denyRestore = false;
    check(!finishPatch(site, trace), "fixture recovery from protection failure");
    expectRestored(site, trace);

    check(!install(site), "install before rollback writable failure");
    denyWrite = true;
    check(finishPatch(site, trace) == ERROR_ACCESS_DENIED && site.ownsBreakpoint && address[0] == 0xCC,
          "persistent writable failure retains known trap and fails cleanup");
    denyWrite = false;
    check(!finishPatch(site, trace), "fixture rolls back retained trap");
    expectRestored(site, trace);

    check(!install(site), "install before persistent rollback flush failure");
    denyFlush = true;
    check(finishPatch(site, trace) == ERROR_WRITE_FAULT && site.needsFlush &&
          trace.finalProtection == PAGE_EXECUTE_READ && address[0] == counterProbeSignature[0],
          "persistent rollback flush failure is exposed even though bytes match");
    denyFlush = false;
    check(!finishPatch(site, trace), "fixture recovers cache flush");
    expectRestored(site, trace);

    check(!install(site), "install before CAS conflict");
    protect(page, PAGE_EXECUTE_READWRITE);
    address[0] = 0x90;
    protect(page, PAGE_EXECUTE_READ);
    check(finishPatch(site, trace) == ERROR_INVALID_DATA && !site.ownsBreakpoint && address[0] == 0x90,
          "rollback reports conflict and preserves third-party opcode");
    protect(page, PAGE_EXECUTE_READWRITE);
    address[0] = counterProbeSignature[0];
    protect(page, PAGE_EXECUTE_READ);
    expectRestored(site, trace);
}

void testHandler(BYTE* address)
{
    deadline.store(0);
    CONTEXT context{};
    context.Rdi = 0x200;
    context.Rax = 0x300;
    context.Rcx = 0x400;
    context.Rip = reinterpret_cast<DWORD64>(address);
    context.EFlags = 0x202;
    context.Rdx = 0x600;
    context.Rsp = 0x700;
    auto expected = context;
    EXCEPTION_RECORD record{};
    record.ExceptionAddress = address;
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    EXCEPTION_POINTERS exception{&record, &context};
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_SEARCH, "ignore non-breakpoint exception");
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = address + 1;
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_SEARCH && !std::memcmp(&context, &expected, sizeof(context)),
          "ignore breakpoint at unrelated address without changing registers");
    record.ExceptionAddress = address;
    expected.Rcx = context.Rdi;
    expected.Rip += 3;
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION &&
          !std::memcmp(&context, &expected, sizeof(context)) && !sampleCount,
          "queued trap emulates only RCX and RIP while collection is disabled");

    deadline.store(tick + 10);
    for (size_t i = 0; i < counterProbeCapacity + 1; ++i) {
        context.Rdi = i;
        check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION, "handle test sample with unmapped pointers");
    }
    context.Rdi = 0;
    onBreakpoint(&exception);
    AcquireSRWLockExclusive(&sampleLock);
    onBreakpoint(&exception);
    ReleaseSRWLockExclusive(&sampleLock);
    tick += 10;
    onBreakpoint(&exception);

    CounterProbeTrace trace;
    check(!copySamples(trace) && trace.sampleCount == counterProbeCapacity &&
          trace.totalCalls == counterProbeCapacity + 3 && trace.droppedCalls == 2 &&
          trace.samples[0].receiver == 0 && trace.samples[0].table == 0x300 &&
          trace.samples[0].thread == GetCurrentThreadId() && trace.samples[0].calls == 2,
          "aggregate bounded samples and account for full and busy drops; omit expired trap");
}

void testExec(BYTE* page, BYTE* address)
{
    using Synthetic = ULONG_PTR (*)(ULONG_PTR, const ULONG_PTR*);
    const auto invoke = reinterpret_cast<Synthetic>(page);
    const ULONG_PTR receiver = 0x12345678;
    std::array<ULONG_PTR, 9> table{};
    table.back() = reinterpret_cast<ULONG_PTR>(page + 128);
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    check(handler != nullptr, "register test callback");
    check(invoke(receiver, table.data()) == receiver, "baseline indirect call");

    PatchSite site{address, PAGE_EXECUTE_READ};
    check(!install(site) && *address == 0xCC, "arm actual INT3");
    deadline.store(tick + 10);
    check(invoke(receiver, table.data()) == receiver && sampleCount == 1 &&
          samples[0].receiver == receiver && samples[0].table == reinterpret_cast<ULONG_PTR>(table.data()) &&
          samples[0].calls == 1, "actual INT3 preserves indirect call and captures numeric registers");
    deadline.store(0);
    CounterProbeTrace trace;
    check(!finishPatch(site, trace), "restore executable fixture");
    expectRestored(site, trace);
    check(invoke(receiver, table.data()) == receiver && sampleCount == 1, "restored fixture executes normally");
    sampleCount = 0;
    samples = {};
    check(RemoveVectoredExceptionHandler(handler) != 0, "remove synchronous fixture callback");
}

} // namespace

int main()
{
    check(SearchCounterProbeAbiV1(nullptr) == 1, "ABI marker");
    CounterProbeTrace input;
    input.version = 0;
    check(RunSearchCounterProbeV1(&input) == ERROR_INVALID_PARAMETER && !handlerAddress.load(),
          "reject invalid version before mutation");
    input.version = counterProbeVersion;
    input.size = sizeof(DWORD) * 2;
    check(RunSearchCounterProbeV1(&input) == ERROR_INVALID_PARAMETER && !handlerAddress.load(),
          "reject truncated ABI before mutation");
    check(RunSearchCounterProbeV1(nullptr) == ERROR_INVALID_PARAMETER, "reject null ABI input");

    auto* page = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate synthetic page");
    // Win64: preserve RDI, allocate call shadow space, move incoming arguments to RDI/RAX.
    constexpr BYTE prologue[]{0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xC2};
    constexpr BYTE epilogue[]{0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3};
    constexpr BYTE callee[]{0x48, 0x8B, 0xC1, 0xC3}; // Return first argument.
    auto* address = page + sizeof(prologue);
    std::memcpy(page, prologue, sizeof(prologue));
    std::memcpy(address, counterProbeSignature.data(), counterProbeSignature.size());
    std::memcpy(address + counterProbeSignature.size(), epilogue, sizeof(epilogue));
    std::memcpy(page + 128, callee, sizeof(callee));
    protect(page, PAGE_EXECUTE_READ);
    check(FlushInstructionCache(GetCurrentProcess(), page, 4096) != FALSE, "publish synthetic code");
    handlerAddress.store(reinterpret_cast<ULONG_PTR>(address));

    CounterProbeTrace alreadyArmed;
    check(RunSearchCounterProbeV1(&alreadyArmed) == ERROR_ALREADY_EXISTS &&
          alreadyArmed.patchError == ERROR_ALREADY_EXISTS, "prevent any re-arm after handler publication");
    testRollback(address);
    testExec(page, address);
    testHandler(address);
    handlerAddress.store(0);
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release synthetic fixture");
    std::puts("PASS: ABI guard, fail-closed rollback, actual INT3 emulation and bounded counter capture");
}
