#include "grid_identity_probe.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
bool denyWrite{};
bool denyRestore{};
bool denyFlush{};
bool failNextFlush{};
ULONGLONG tick = 100;
ULONG_PTR frameBase{};
constexpr ULONG_PTR frontendBase = 0x112233440000;
constexpr DWORD providerRva = 0xD62048;
constexpr DWORD rowsRva = 0xD61F70;
ULONG_PTR watchedField{};
DWORD fieldQueries{};

SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T size)
{
    const auto value = reinterpret_cast<ULONG_PTR>(address);
    if (watchedField && value == watchedField) ++fieldQueries;
    const auto rva = value - frameBase;
    const auto frontendRva = value - frontendBase;
    const bool frameTable = frameBase && (rva == gridStandaloneTableRva || rva == gridAggregatedTableRva);
    if (frameTable || (frontendRva >= providerRva && frontendRva < rowsRva + 0x100)) {
        *region = {};
        region->AllocationBase = reinterpret_cast<LPVOID>(frameTable ? frameBase : frontendBase);
        region->BaseAddress = const_cast<LPVOID>(address);
        region->RegionSize = sizeof(ULONG_PTR);
        region->State = MEM_COMMIT;
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_READONLY;
        return sizeof(*region);
    }
    return VirtualQuery(address, region, size);
}

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

#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#define GetTickCount64 fixtureTick
#include "grid_identity_probe.cpp"
#undef GetTickCount64
#undef FlushInstructionCache
#undef VirtualProtect
#undef VirtualQuery

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

void expectRestored(const PatchSite& site, const GridProbeTrace& trace)
{
    check(!site.ownsBreakpoint && !site.needsFlush && !site.restoreProtection &&
          trace.finalProtection == PAGE_EXECUTE_READ &&
          std::memcmp(site.address, gridProbeSignature.data(), gridProbeSignature.size()) == 0,
          "restore complete signature and original protection");
}

void testRollback(BYTE* address)
{
    auto* page = address; // A protection change rounds this address down to its page.
    PatchSite site{address, PAGE_EXECUTE_READ};
    GridProbeTrace trace;
    denyWrite = true;
    check(install(site) == ERROR_ACCESS_DENIED && !finishPatch(site, trace), "reject initial writable failure");
    denyWrite = false;
    expectRestored(site, trace);

    protect(page, PAGE_EXECUTE_READWRITE);
    address[gridProbeSignature.size() - 1] ^= 1;
    protect(page, PAGE_EXECUTE_READ);
    check(install(site) == ERROR_INVALID_DATA && address[0] == gridProbeSignature[0],
          "full signature checked at patch time");
    protect(page, PAGE_EXECUTE_READWRITE);
    address[gridProbeSignature.size() - 1] ^= 1;
    protect(page, PAGE_EXECUTE_READ);

    failNextFlush = true;
    check(install(site) == ERROR_WRITE_FAULT && site.ownsBreakpoint &&
          !finishPatch(site, trace), "rollback after initial flush failure");
    expectRestored(site, trace);

    denyRestore = true;
    check(install(site) == ERROR_ACCESS_DENIED && site.ownsBreakpoint && site.restoreProtection,
          "installation reports protection failure");
    check(finishPatch(site, trace) == ERROR_ACCESS_DENIED &&
          trace.finalProtection == PAGE_EXECUTE_READWRITE && address[0] == gridProbeSignature[0],
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
          trace.finalProtection == PAGE_EXECUTE_READ && address[0] == gridProbeSignature[0],
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
    address[0] = gridProbeSignature[0];
    protect(page, PAGE_EXECUTE_READ);
    expectRestored(site, trace);
}

void testReadLimits()
{
    ULONG_PTR value{};
    check(!readPointer(0, gridProviderField, value) &&
          !readPointer(~ULONG_PTR{0}, 8, value) && !readPointer(1, 0, value),
          "null, overflow and unaligned pointer reads rejected");
    check(!readWrapperReturn(0, value) && !readWrapperReturn(~ULONG_PTR{0}, value),
          "invalid stack range rejected before dereference");
    auto* page = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate protection fixture");
    protect(page, PAGE_READWRITE | PAGE_GUARD);
    check(!readPointer(reinterpret_cast<ULONG_PTR>(page), 0, value), "guard read rejected");
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "guard preserved");
    protect(page, PAGE_NOACCESS);
    check(captureIdentity(reinterpret_cast<ULONG_PTR>(page), frameBase).root.state == GridTableState::ReadFailed,
          "inaccessible root classified without fault");
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release protection fixture");

    std::array<ULONG_PTR, 264> root{};
    auto* control = root.data();
    const auto receiver = reinterpret_cast<ULONG_PTR>(control);
    root[0] = frontendBase + providerRva;
    watchedField = receiver + gridProviderField;
    const auto unknown = captureIdentity(receiver, frameBase);
    check(unknown.kind == GridRootKind::Unknown && unknown.root.state == GridTableState::ImageTable &&
          unknown.provider.state == GridTableState::NotRead && !fieldQueries,
          "foreign table never authorizes an interior object read");
    root[0] = frameBase + gridAggregatedTableRva;
    root[gridProviderField / sizeof(ULONG_PTR)] = 1;
    const auto failed = captureIdentity(receiver, frameBase);
    check(fieldQueries == 1 && failed.kind == GridRootKind::Aggregated &&
          failed.provider.state == GridTableState::ReadFailed && failed.rows.state == GridTableState::Null &&
          failed.site.state == GridTableState::NotRead && failed.alias == GridRowsAlias::Unknown,
          "known aggregated root exposes bounded read failures but no site read");
    watchedField = 0;
}

void testHandler(BYTE* address)
{
    CONTEXT context{};
    context.Rdi = 0x123;
    context.Rcx = 0x456;
    context.Rip = reinterpret_cast<ULONG_PTR>(address);
    context.EFlags = 0x202;
    context.Rbx = 0x789;
    auto expected = context;
    EXCEPTION_RECORD record{};
    record.ExceptionAddress = address;
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    EXCEPTION_POINTERS exception{&record, &context};
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_SEARCH, "ignore non-breakpoint exception");
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = address + 1;
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_SEARCH && !std::memcmp(&context, &expected, sizeof(context)),
          "ignore unrelated breakpoint without register modifications");
    record.ExceptionAddress = address;
    expected.Rdi = context.Rcx;
    expected.Rip += 3;
    deadline.store(tick);
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION &&
          !std::memcmp(&context, &expected, sizeof(context)) && !sampleCount && !stackReadFailures,
          "expired trap emulates only RDI and RIP without reading the stack or changing flags");
    deadline.store(tick + 10);
    check(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION && stackReadFailures == 1 && !sampleCount,
          "active trap with unknown stack preserves emulation and counts failure");
    std::array<ULONG_PTR, gridWrapperStackOffset / sizeof(ULONG_PTR) + 1> stack{};
    stack.back() = frameBase + gridWrapperReturnRva;
    context.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
    context.Rcx = ~ULONG_PTR{0} & ~(alignof(ULONG_PTR) - 1);
    MEMORY_BASIC_INFORMATION region{};
    check(!VirtualQuery(reinterpret_cast<LPCVOID>(context.Rcx), &region, sizeof(region)),
          "unmapped pointer fixture causes an actual VirtualQuery failure");
    constexpr DWORD lastErrorSentinel = 0xFACE1234;
    SetLastError(lastErrorSentinel);
    const auto replayed = onBreakpoint(&exception);
    const auto lastError = GetLastError();
    check(replayed == EXCEPTION_CONTINUE_EXECUTION && lastError == lastErrorSentinel &&
          sampleCount == 1 && samples[0].identity.root.state == GridTableState::ReadFailed,
          "failed collector read preserves the interrupted thread's last error");
    sampleCount = 0;
    samples = {};
    deadline.store(0);
}

void testExec(BYTE* page, BYTE* address)
{
    using Synthetic = ULONG_PTR (*)(ULONG_PTR, ULONG_PTR);
    const auto invoke = reinterpret_cast<Synthetic>(page);
    const auto caller = frameBase + gridWrapperReturnRva;
    // The instruction following the mov inspects RCX+A88 in the real signature.
    std::array<ULONG_PTR, 338> root{}, secondRoot{};
    std::array<ULONG_PTR, 4> provider{frontendBase + providerRva, 0, 0, frontendBase + rowsRva};
    ULONG_PTR site = frontendBase + rowsRva + 8;
    const auto receiver = reinterpret_cast<ULONG_PTR>(root.data());
    const auto secondReceiver = reinterpret_cast<ULONG_PTR>(secondRoot.data());
    root[0] = frameBase + gridStandaloneTableRva;
    root[gridProviderField / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(provider.data());
    root[gridRowsField / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(provider.data() + 3);
    root[gridSiteField / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(&site);
    secondRoot = root;
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    check(handler != nullptr && invoke(receiver, caller) == receiver, "register VEH, baseline synthetic paint");
    PatchSite patch{address, PAGE_EXECUTE_READ};
    check(!install(patch) && *address == 0xCC, "arm actual INT3");
    deadline.store(tick + 10);
    check(invoke(receiver, caller + 1) == receiver && otherCallerCalls == 1 && !sampleCount,
          "paint wrapper gate rejects other call sites");
    check(invoke(receiver, caller) == receiver && invoke(secondReceiver, caller) == secondReceiver &&
          sampleCount == 1 && samples[0].calls == 2, "actual INT3 emulates mov and aggregates identities across objects");
    const auto id = samples[0].identity;
    check(id.kind == GridRootKind::Standalone && id.alias == GridRowsAlias::ProviderPlusOffset &&
          id.provider.rva == providerRva && id.rows.rva == rowsRva && id.site.rva == rowsRva + 8,
          "actual breakpoint observes root, provider, rows, alias and site");
    root[gridRowsField / sizeof(ULONG_PTR)] = root[gridProviderField / sizeof(ULONG_PTR)];
    check(invoke(receiver, caller) == receiver && sampleCount == 2 &&
          samples[1].identity.alias == GridRowsAlias::Same && samples[1].identity.rows.rva == providerRva,
          "reused object address is read anew");
    for (ULONG_PTR delta = 0; delta < gridProbeCapacity; ++delta) {
        site = frontendBase + rowsRva + 16 + delta * sizeof(ULONG_PTR);
        check(invoke(receiver, caller) == receiver, "emulate with newly distinct identity");
    }
    AcquireSRWLockExclusive(&sampleLock);
    check(invoke(receiver, caller) == receiver, "contended handler remains nonblocking");
    ReleaseSRWLockExclusive(&sampleLock);
    tick += 10;
    check(invoke(receiver, caller) == receiver, "queued trap after deadline still emulates");
    deadline.store(0);
    GridProbeTrace trace;
    check(!finishPatch(patch, trace) && !copySamples(trace), "restore executable fixture before copying");
    expectRestored(patch, trace);
    check(invoke(receiver, caller) == receiver && trace.sampleCount == gridProbeCapacity &&
          trace.droppedCalls == 3 && trace.stackReadFailures == 1 && trace.otherCallerCalls == 1 &&
          trace.totalCalls == gridProbeCapacity + 6 && trace.samples[0].calls == 2,
          "bounded aggregation exposes full-cache, contention and stack losses");
    check(RemoveVectoredExceptionHandler(handler) != 0, "remove synchronous test callback");
}

} // namespace

int main()
{
    check(GridIdentityProbeAbiV1(nullptr) == gridProbeVersion, "ABI marker");
    GridProbeTrace input;
    input.version = 0;
    check(RunGridIdentityProbeV1(&input) == ERROR_INVALID_PARAMETER && !attempted,
          "reject invalid version before taking the one-shot slot");
    input.version = gridProbeVersion;
    input.size = sizeof(DWORD) * 2;
    check(RunGridIdentityProbeV1(&input) == ERROR_INVALID_PARAMETER && !attempted, "reject truncated ABI");
    check(RunGridIdentityProbeV1(nullptr) == ERROR_INVALID_PARAMETER, "reject null ABI input");
    attempted = true;
    input.size = sizeof(input);
    check(RunGridIdentityProbeV1(&input) == ERROR_ALREADY_EXISTS &&
          input.patchError == ERROR_ALREADY_EXISTS && !handlerAddress.load(), "one-shot slot cannot rearm");

    auto* page = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate synthetic page");
    constexpr BYTE prologue[]{
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,                   // align synthetic image base
        0x57, 0x41, 0x54,                                     // push rdi, r12
        0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00,             // sub rsp, D8
        0x48, 0x89, 0x94, 0x24, 0xC8, 0x00, 0x00, 0x00};      // mov [rsp+C8], rdx
    constexpr BYTE epilogue[]{
        0x48, 0x8B, 0xC7,                                     // mov rax, rdi
        0x48, 0x81, 0xC4, 0xD8, 0x00, 0x00, 0x00,             // add rsp, D8
        0x41, 0x5C, 0x5F, 0xC3};                             // pop r12, rdi; ret
    constexpr BYTE afterSignature[]{
        0x81, 0xC1, 0x50, 0x0E, 0x00, 0x00,                   // complete add rcx, E50
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,                   // pad to the je target
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static_assert(gridProbeSignature.size() + sizeof(afterSignature) == 15 + 0x13);
    auto* address = page + sizeof(prologue);
    std::memcpy(page, prologue, sizeof(prologue));
    std::memcpy(address, gridProbeSignature.data(), gridProbeSignature.size());
    auto* continuation = address + gridProbeSignature.size();
    std::memcpy(continuation, afterSignature, sizeof(afterSignature));
    std::memcpy(continuation + sizeof(afterSignature), epilogue, sizeof(epilogue));
    protect(page, PAGE_EXECUTE_READ);
    check(FlushInstructionCache(GetCurrentProcess(), page, 4096) != FALSE, "publish synthetic code");
    frameBase = reinterpret_cast<ULONG_PTR>(address) - gridProbeRva;
    handlerAddress.store(reinterpret_cast<ULONG_PTR>(address));

    testRollback(address);
    testReadLimits();
    testHandler(address);
    testExec(page, address);
    handlerAddress.store(0);
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release synthetic fixture");
    std::puts("PASS: grid ABI, checked reads, live identity, wrapper gate, actual INT3 and rollback failures");
}
