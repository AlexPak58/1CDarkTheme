#include "dock_fill_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
BYTE* deniedWrite{};
BYTE* deniedRestore{};
BYTE* deniedFlush{};
LPCVOID deniedSlot{};
ULONGLONG tick = 100;
constexpr DWORD firstFixtureRva = 0xB2E000;
alignas(ULONG_PTR) std::array<ULONG_PTR, 0x5000 / sizeof(ULONG_PTR)> dockImage{};
const auto fakeFrame = reinterpret_cast<ULONG_PTR>(dockImage.data()) - firstFixtureRva;

BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous)
{
    if ((deniedWrite && address == deniedWrite && protection == PAGE_EXECUTE_READWRITE) ||
        (deniedRestore && address == deniedRestore && protection == PAGE_EXECUTE_READ)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return VirtualProtect(address, size, protection, previous);
}

BOOL WINAPI fixtureFlush(HANDLE process, LPCVOID address, SIZE_T size)
{
    if (deniedFlush && address == deniedFlush) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushInstructionCache(process, address, size);
}

ULONGLONG WINAPI fixtureTick() { return tick; }

SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    const auto result = VirtualQuery(address, region, length);
    const auto value = reinterpret_cast<ULONG_PTR>(address);
    if (result && value >= fakeFrame + firstFixtureRva &&
        value - (fakeFrame + firstFixtureRva) < sizeof(dockImage)) {
        region->AllocationBase = reinterpret_cast<LPVOID>(fakeFrame);
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_READONLY;
    }
    if (result && address == deniedSlot) region->Protect = PAGE_NOACCESS;
    return result;
}
}

#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#define GetTickCount64 fixtureTick
#include "dock_fill_probe.cpp"
#undef GetTickCount64
#undef FlushInstructionCache
#undef VirtualProtect
#undef VirtualQuery

namespace {
constexpr DockProbeRect fullRect{-10, 15, 550, 31};
constexpr DockProbeRect clippedRect{0, 20, 340, 31};
constexpr ULONG_PTR brush = 0x123456789ABC;
constexpr DWORD rawColor = 0x77D2D9E6;

ULONG_PTR returnRcx(void* object) { return reinterpret_cast<ULONG_PTR>(object); }
using DrawMethod = ULONG_PTR (*)(void*);

ULONG_PTR& methodSlot(const DockOwnerDefinition& owner)
{
    return dockImage[(owner.drawRva + 0x18 - firstFixtureRva) / sizeof(ULONG_PTR)];
}

void check(bool okay, const char* reason)
{
    if (!okay) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}

void protection(void* address, DWORD flags)
{
    DWORD previous{};
    check(VirtualProtect(address, 1, flags, &previous) != FALSE, "fixture protection");
}

struct DockFixture {
    std::array<ULONG_PTR, 0x290 / 8> root{};

    explicit DockFixture(const DockOwnerDefinition& owner)
    {
        root[0] = fakeFrame + owner.rootRva;
        root[0xB0 / 8] = fakeFrame + owner.sideRva;
        root[0x188 / 8] = fakeFrame + owner.drawRva;
        methodSlot(owner) = fakeFrame + dockSites[static_cast<size_t>(owner.site)].drawMethodRva;
        std::memcpy(&root[0x208 / 8], &fullRect, sizeof(fullRect));
        root[0x268 / 8] = rawColor;
        root[0x270 / 8] = brush;
        root[0x288 / 8] = 0x10;
    }

    ULONG_PTR drawInterface() const { return reinterpret_cast<ULONG_PTR>(&root[0x188 / 8]); }
};

using FixtureStack = std::array<ULONG_PTR, 0x120 / 8>;
CONTEXT contextFor(DockSite site, const DockFixture& fixture, FixtureStack& stack)
{
    CONTEXT context{};
    context.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
    context.EFlags = 0x203;
    context.R8 = brush;
    std::memcpy(&stack[0x40 / 8], &clippedRect, sizeof(clippedRect));
    if (site == DockSite::Basic) {
        stack[0x88 / 8] = reinterpret_cast<ULONG_PTR>(&contextFor);
        context.Rbx = fixture.drawInterface();
        context.Rbp = 0x11223344;
    } else {
        stack[0xA8 / 8] = reinterpret_cast<ULONG_PTR>(&returnRcx);
        context.R14 = fixture.drawInterface();
        context.Rbp = context.Rsp + 0x80;
        context.Rdx = context.R15 = 0x55667788;
        context.Rdi = 0x77889900;
    }
    return context;
}

void testCapture()
{
    FixtureStack stack{};
    DockIdentity id{};
    for (const auto& owner : dockOwners) {
        DockFixture fixture(owner);
        auto context = contextFor(owner.site, fixture, stack);
        check(captureIdentity(context, fakeFrame, owner.site, id) == Observation::Accepted &&
              id.rootTypeRva == owner.rootRva && id.caller.state == DockLocationState::Image &&
              id.full == fullRect && id.clipped == clippedRect && id.rawColor == rawColor &&
              id.orientationRaw == (owner.site == DockSite::Basic ? 0UL : 0x10UL),
              "exact types, clipped and full geometry, raw color and raw orientation collected together");
        for (const auto offset : {0, 0xB0, 0x188}) {
            ++fixture.root[offset / 8];
            check(captureIdentity(context, fakeFrame, owner.site, id) == Observation::Mismatch,
                  "any incompatible table in the exact owner triple is rejected");
            --fixture.root[offset / 8];
        }
        ++methodSlot(owner);
        check(captureIdentity(context, fakeFrame, owner.site, id) == Observation::Mismatch,
              "proven draw table with wrong +18 slot rejected");
        --methodSlot(owner);
        deniedSlot = &methodSlot(owner);
        check(captureIdentity(context, fakeFrame, owner.site, id) == Observation::ReadFailure,
              "unreadable +18 slot fails closed");
        deniedSlot = nullptr;
        ++context.R8;
        check(captureIdentity(context, fakeFrame, owner.site, id) == Observation::Mismatch,
              "recorded brush must be the one passed in R8");
    }

    DockFixture basic(dockOwners[0]), edge(dockOwners[2]);
    auto context = contextFor(DockSite::WithEdge, edge, stack);
    context.R14 = basic.drawInterface();
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::Mismatch,
          "valid basic family is rejected at the edge branch");
    context.R14 = edge.drawInterface();
    ++context.Rbp;
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::Mismatch,
          "clipped RECT frame pointer must be the preserved frame register");
    --context.Rbp;
    ++context.R15;
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::Mismatch,
          "edge branch VDC copied to RDX must match R15");
    --context.R15;
    for (const DockProbeRect bad : {DockProbeRect{0, 20, 600, 31}, DockProbeRect{0, 20, 0, 31}}) {
        std::memcpy(&stack[0x40 / 8], &bad, sizeof(bad));
        check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::Mismatch,
              "empty or out-of-full-rect clipped geometry rejected");
    }
    context = contextFor(DockSite::WithEdge, edge, stack);
    const auto knownCaller = stack[0xA8 / 8];
    stack[0xA8 / 8] = context.Rsp;
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::Accepted &&
          id.caller.state == DockLocationState::Unmapped && !id.caller.imageBase && !id.caller.rva,
          "stack caller value is not serialized as a raw address");
    stack[0xA8 / 8] = knownCaller;
    context.Rsp = ~ULONG_PTR{7};
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::ReadFailure,
          "overflowing stack-relative offset rejected");

    auto* const page = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate guarded root");
    std::memcpy(page, edge.root.data(), sizeof(edge.root));
    protection(page, PAGE_READWRITE | PAGE_GUARD);
    context = contextFor(DockSite::WithEdge, edge, stack);
    context.R14 = reinterpret_cast<ULONG_PTR>(page) + 0x188;
    MEMORY_BASIC_INFORMATION region{};
    check(captureIdentity(context, fakeFrame, DockSite::WithEdge, id) == Observation::ReadFailure &&
          VirtualQuery(page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
          "reject guarded root memory without consuming the guard");
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release guarded root");
    const auto clippedAddress = reinterpret_cast<ULONG_PTR>(&stack[0x40 / 8]);
    StackRange shortStack{clippedAddress, clippedAddress + sizeof(DockProbeRect) - sizeof(LONG)};
    DockProbeRect rectangle{};
    check(!shortStack.read(clippedAddress, rectangle), "reject clipped RECT spanning beyond readable stack range");
}

void testReplayAndCounts()
{
    DockFixture basic(dockOwners[0]), edge(dockOwners[2]);
    FixtureStack stack{};
    for (size_t i = 0; i < dockSites.size(); ++i) {
        handlerAddresses[i].store(fakeFrame + dockSites[i].rva);
        replaySites[i].store(true);
    }
    const auto replay = [](CONTEXT& context, DockSite site) {
        EXCEPTION_RECORD record{};
        record.ExceptionCode = EXCEPTION_BREAKPOINT;
        record.ExceptionAddress = reinterpret_cast<LPVOID>(fakeFrame + dockSites[static_cast<size_t>(site)].rva);
        CONTEXT expected = context;
        if (site == DockSite::Basic) expected.Rdx = context.Rbp;
        else expected.Rcx = context.Rdi;
        expected.Rip = reinterpret_cast<ULONG_PTR>(record.ExceptionAddress) + 3;
        EXCEPTION_POINTERS exception{&record, &context};
        constexpr DWORD sentinel = 0xFACE1234;
        SetLastError(sentinel);
        const auto result = onBreakpoint(&exception);
        const auto lastError = GetLastError();
        check(result == EXCEPTION_CONTINUE_EXECUTION && lastError == sentinel &&
              !std::memcmp(&context, &expected, sizeof(context)),
              "replay changes only the prescribed register and RIP, and preserves LastError");
    };
    auto bg = contextFor(DockSite::Basic, basic, stack);
    replay(bg, DockSite::Basic);
    check(!sampleCount, "inactive retained replay gathers nothing");
    deadline.store(tick + 10);
    replay(bg, DockSite::Basic);
    replay(bg, DockSite::Basic);
    auto withEdge = contextFor(DockSite::WithEdge, edge, stack);
    replay(withEdge, DockSite::WithEdge);
    stack[0xA8 / 8] = withEdge.Rsp;
    replay(withEdge, DockSite::WithEdge);
    ++withEdge.R8;
    replay(withEdge, DockSite::WithEdge);
    --withEdge.R8;
    withEdge.R14 = 0xFFFF800000000000;
    replay(withEdge, DockSite::WithEdge);
    AcquireSRWLockExclusive(&sampleLock);
    replay(withEdge, DockSite::WithEdge);
    ReleaseSRWLockExclusive(&sampleLock);
    for (DWORD value = 1; value <= dockProbeCapacity; ++value) {
        basic.root[0x268 / 8] = value;
        replay(bg, DockSite::Basic);
    }
    tick += 10;
    replay(bg, DockSite::Basic);
    deadline.store(0);
    DockProbeTrace trace;
    check(!copySamples(trace) && trace.sampleCount == dockProbeCapacity && trace.samples[0].calls == 2 &&
          trace.counts[0].accepted == dockProbeCapacity - 1 && trace.counts[0].capacityDrops == 3 &&
          trace.counts[1].accepted == 2 && trace.counts[1].unknownIdentities == 1 &&
          trace.counts[1].mismatches == 1 && trace.counts[1].readFailures == 1 && trace.counts[1].busyDrops == 1,
          "bounded aggregation and distinct mismatch, read failure, unknown and contention counts");
    for (auto& active : replaySites) active.store(false);
}

struct ExecutableFixture {
    BYTE* memory{};
    std::array<BYTE*, 2> entry{};
    PatchSites sites{};

    ExecutableFixture()
    {
        constexpr SIZE_T codeSize = 0x4A1000;
        memory = static_cast<BYTE*>(VirtualAlloc(nullptr, codeSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(memory != nullptr, "allocate synthetic instructions");
        entry = {memory, memory + 4096};

        // Preserve RBP/RSI and return replayed RDX after the exact signature.
        constexpr BYTE firstPrologue[]{
            0x55, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xE9, 0x33, 0xD2};
        constexpr BYTE firstEpilogue[]{
            0x49, 0x00, 0x48, 0x8B, 0xC2, 0x48, 0x8B, 0xB4, 0x24, 0xA0, 0x00, 0x00, 0x00,
            0x48, 0x81, 0xC4, 0xB0, 0x00, 0x00, 0x00, 0x5D, 0xC3};
        auto* const first = entry[0] + sizeof(firstPrologue);
        std::memcpy(entry[0], firstPrologue, sizeof(firstPrologue));
        std::memcpy(first, dockSites[0].signature.data(), dockSites[0].signature.size());
        std::memcpy(first + dockSites[0].signature.size(), firstEpilogue, sizeof(firstEpilogue));

        // Supply RDI and a synthetic object with method +230 that returns RCX.
        constexpr BYTE secondPrologue[]{
            0x55, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8D, 0x6C, 0x24, 0x38,
            0x48, 0x8B, 0xF9, 0x33, 0xC9, 0x89, 0x0C, 0x24};
        constexpr BYTE secondEpilogue[]{0x83, 0xC4, 0x38, 0x5F, 0x5D, 0xC3};
        auto* const second = entry[1] + sizeof(secondPrologue);
        std::memcpy(entry[1], secondPrologue, sizeof(secondPrologue));
        std::memcpy(second, dockSites[1].signature.data(), dockSites[1].signature.size());
        std::memcpy(second + dockSites[1].signature.size(), secondEpilogue, sizeof(secondEpilogue));
        sites = {{{first, PAGE_EXECUTE_READ}, {second, PAGE_EXECUTE_READ}}};
        for (size_t i = 0; i < sites.size(); ++i) {
            protection(sites[i].address, PAGE_EXECUTE_READ);
            handlerAddresses[i].store(reinterpret_cast<ULONG_PTR>(sites[i].address));
        }
        check(FlushInstructionCache(GetCurrentProcess(), memory, 8192) != FALSE, "publish synthetic code");
    }

    ~ExecutableFixture()
    {
        check(VirtualFree(memory, 0, MEM_RELEASE) != FALSE, "release synthetic instructions");
    }

    void expectRestored(const DockProbeTrace& trace)
    {
        for (size_t i = 0; i < sites.size(); ++i)
            check(!sites[i].ownsBreakpoint && !sites[i].needsFlush && !sites[i].restoreProtection &&
                  trace.patches[i].finalProtection == PAGE_EXECUTE_READ && signatureMatches(sites[i].address, i),
                  "full opcode and page protection restored for each site");
    }
};

void testTransaction(ExecutableFixture& fixture)
{
    auto& sites = fixture.sites;
    DockProbeTrace trace;
    deniedWrite = sites[1].address;
    check(installAll(sites) == ERROR_ACCESS_DENIED && sites[0].ownsBreakpoint && !sites[1].ownsBreakpoint &&
          !finishAll(sites, trace), "second write failure rolls back first site only");
    deniedWrite = nullptr;
    fixture.expectRestored(trace);

    auto* const tail = sites[1].address + dockSites[1].signature.size() - 1;
    protection(tail, PAGE_EXECUTE_READWRITE);
    *tail ^= 1;
    protection(tail, PAGE_EXECUTE_READ);
    check(installAll(sites) == ERROR_INVALID_DATA && sites[0].ownsBreakpoint && !sites[1].ownsBreakpoint,
          "second layout drift at final signature byte rejected");
    check(finishAll(sites, trace) == ERROR_INVALID_DATA && !trace.patches[0].cleanupError &&
          trace.patches[1].cleanupError == ERROR_INVALID_DATA,
          "per-site drift is exposed while first page is restored");
    protection(tail, PAGE_EXECUTE_READWRITE);
    *tail ^= 1;
    protection(tail, PAGE_EXECUTE_READ);
    fixture.expectRestored(trace);

    deniedRestore = sites[1].address;
    check(installAll(sites) == ERROR_ACCESS_DENIED && sites[1].ownsBreakpoint &&
          finishAll(sites, trace) == ERROR_ACCESS_DENIED && !trace.patches[0].cleanupError &&
          trace.patches[0].finalProtection == PAGE_EXECUTE_READ &&
          trace.patches[1].cleanupError == ERROR_ACCESS_DENIED &&
          trace.patches[1].finalProtection == PAGE_EXECUTE_READWRITE,
          "one unresolved protection never skips restoring the other page");
    deniedRestore = nullptr;
    check(!finishAll(sites, trace), "test fixture recovers protection");
    fixture.expectRestored(trace);

    check(!installAll(sites), "arm before simulated cache failure");
    deniedFlush = sites[1].address;
    check(finishAll(sites, trace) == ERROR_WRITE_FAULT && sites[1].needsFlush &&
          !trace.patches[0].cleanupError && trace.patches[1].cleanupError == ERROR_WRITE_FAULT,
          "report cache ambiguity even with correct bytes and protection");
    deniedFlush = nullptr;
    check(!finishAll(sites, trace), "retry test fixture cache flush");

    check(!installAll(sites), "arm before CAS conflict");
    protection(sites[0].address, PAGE_EXECUTE_READWRITE);
    *sites[0].address = 0x90;
    protection(sites[0].address, PAGE_EXECUTE_READ);
    check(finishAll(sites, trace) == ERROR_INVALID_DATA && *sites[0].address == 0x90 &&
          !trace.patches[1].cleanupError, "preserve another opcode and restore the second site");
    protection(sites[0].address, PAGE_EXECUTE_READWRITE);
    *sites[0].address = dockSites[0].signature.front();
    protection(sites[0].address, PAGE_EXECUTE_READ);
    fixture.expectRestored(trace);
}

void testActualTraps(ExecutableFixture& fixture)
{
    using Synthetic = ULONG_PTR (*)(void*);
    const auto first = reinterpret_cast<Synthetic>(fixture.entry[0]);
    const auto second = reinterpret_cast<Synthetic>(fixture.entry[1]);
    std::array<DrawMethod, 0x238 / 8> table{};
    table[0x230 / 8] = returnRcx;
    auto object = table.data();
    const auto objectAddress = reinterpret_cast<ULONG_PTR>(&object);
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    check(handler && first(&object) == objectAddress && second(&object) == objectAddress,
          "baseline exact instruction windows execute");
    check(!installAll(fixture.sites) && *fixture.sites[0].address == 0xCC &&
          *fixture.sites[1].address == 0xCC && first(&object) == objectAddress && second(&object) == objectAddress,
          "first INT3 replays RDX and second INT3 replays RCX into executed synthetic code");
    DockProbeTrace trace;
    check(!finishAll(fixture.sites, trace) && first(&object) == objectAddress && second(&object) == objectAddress,
          "normal execution after two-site rollback");
    fixture.expectRestored(trace);
    check(RemoveVectoredExceptionHandler(handler) != 0, "remove test-only callback after synchronous replay");
}
}

int main()
{
    DockProbeTrace trace;
    check(DockFillProbeAbiV1(nullptr) == dockProbeVersion, "dock export ABI");
    trace.version = 0;
    check(RunDockFillProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "invalid ABI leaves one-shot unused");
    trace.version = dockProbeVersion;
    trace.size = sizeof(DWORD) * 2;
    check(RunDockFillProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "reject truncated input before mutation");
    trace.size = sizeof(trace);
    attempted = true;
    check(RunDockFillProbeV1(&trace) == ERROR_ALREADY_EXISTS, "attempt cannot re-arm");
    testCapture();
    testReplayAndCounts();
    ExecutableFixture fixture;
    testTransaction(fixture);
    testActualTraps(fixture);
    std::puts("PASS: strict dock owners and geometry, dual INT3 replay, rollback and bounded collection");
}
