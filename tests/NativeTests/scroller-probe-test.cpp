#include "scroller_identity_probe.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
BYTE* deniedWrite{};
BYTE* deniedRestore{};
BYTE* deniedFlush{};
LPCVOID deniedSlot{};
ULONGLONG tick = 100;
alignas(ULONG_PTR) std::array<ULONG_PTR, 0x56000 / sizeof(ULONG_PTR)> scrollerImage{};
const auto fakeWbase = reinterpret_cast<ULONG_PTR>(scrollerImage.data());

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
    if (result && value >= fakeWbase + 0x19000 && value < fakeWbase + 0x1C000) {
        region->AllocationBase = scrollerImage.data();
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_EXECUTE_READ;
    } else if (result && value >= fakeWbase + 0x3C000 && value < fakeWbase + 0x3E000) {
        region->AllocationBase = scrollerImage.data();
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_READONLY;
    } else if (result && value >= fakeWbase + 0x55000 && value < fakeWbase + 0x56000) {
        region->AllocationBase = scrollerImage.data();
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_READWRITE;
    }
    if (result && address == deniedSlot) region->Protect = PAGE_NOACCESS;
    return result;
}
}

#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#define GetTickCount64 fixtureTick
#include "scroller_identity_probe.cpp"
#undef GetTickCount64
#undef FlushInstructionCache
#undef VirtualProtect
#undef VirtualQuery

namespace {
constexpr DWORD fixtureTableRva = 0x3CA78;
const ULONG_PTR windowTable[]{0x101};
ULONG_PTR returnRdx(void*, ULONG_PTR arg) { return arg; }
using DrawMethod = ULONG_PTR (*)(void*, ULONG_PTR);
const DrawMethod drawTable[]{nullptr, nullptr, returnRdx};
constexpr ScrollerProbeRect dispatchRect{-10, 15, 550, 31};
constexpr ScrollerProbeRect componentRect{20, -2, 470, 21};

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

struct ScrollerFixture {
    std::array<ULONG_PTR, 0x28 / 8> scroller{};
    std::array<ULONG_PTR, 0xC0 / 8> window{};
    std::array<ULONG_PTR, 0x80 / 8> geometry{};
    ScrollerProbeRect input{dispatchRect};
    ULONG_PTR draw = reinterpret_cast<ULONG_PTR>(drawTable);

    ScrollerFixture()
    {
        for (const auto& table : scrollerTables) {
            scrollerImage[(table.tableRva + 0x58) / 8] = fakeWbase + scrollerEntryRva;
            scrollerImage[(table.tableRva + 0xC0) / 8] = fakeWbase + table.drawRva;
        }
        scroller[0] = fakeWbase + fixtureTableRva;
        scroller[0x20 / 8] = reinterpret_cast<ULONG_PTR>(window.data());
        window[0] = reinterpret_cast<ULONG_PTR>(windowTable);
        window[0xB8 / 8] = address();
        geometry[0x28 / 8] = reinterpret_cast<ULONG_PTR>(&draw);
        std::memcpy(&geometry[0x50 / 8], &componentRect, sizeof(componentRect));
    }

    ULONG_PTR address() const { return reinterpret_cast<ULONG_PTR>(scroller.data()); }
};

CONTEXT contextFor(ScrollerSite site, ScrollerFixture& fixture, std::array<ULONG_PTR, 0x200 / 8>& stack)
{
    CONTEXT context{};
    context.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
    context.EFlags = 0x203;
    if (site == ScrollerSite::Dispatch) {
        stack[0x108 / 8] = reinterpret_cast<ULONG_PTR>(&contextFor);
        context.Rax = fixture.scroller[0];
        context.Rsi = fixture.address();
        context.Rbp = reinterpret_cast<ULONG_PTR>(&fixture.input);
        context.R8 = context.R9 = context.Rbp;
        context.R14 = 0x11223344;
    } else {
        stack[0x88 / 8] = reinterpret_cast<ULONG_PTR>(&returnRdx);
        context.Rbp = 0x55667788;
        context.R14 = fixture.address();
        context.Rdi = 0x50;
        context.R15 = reinterpret_cast<ULONG_PTR>(fixture.geometry.data());
        context.Rbx = context.R15 + 0x28;
        context.Rcx = reinterpret_cast<ULONG_PTR>(&fixture.draw);
        context.R8 = context.Rsi = context.R15 + context.Rdi;
        context.R9 = fixture.address() + 0x28;
        context.R12 = 0x234;
    }
    return context;
}

void testCapture()
{
    // Dispatch RECT and component layout may reside outside the active stack.
    auto fixture = std::make_unique<ScrollerFixture>();
    std::array<ULONG_PTR, 0x200 / 8> stack{};
    auto dispatch = contextFor(ScrollerSite::Dispatch, *fixture, stack);
    ScrollerIdentity id{};
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Accepted &&
          id.window.state == ScrollerLocationState::Image && id.caller.state == ScrollerLocationState::Image &&
          id.scroller.rva == fixtureTableRva && id.dispatchMethodRva == scrollerCommonDrawRva &&
          id.rect == dispatchRect && id.component.state == ScrollerLocationState::NotRead &&
          id.png == ScrollerPngObservation{},
          "collect dispatch image identities and exact non-stack input rectangle");
    for (const auto& table : scrollerTables) {
        dispatch.Rax = fixture->scroller[0] = fakeWbase + table.tableRva;
        check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Accepted &&
              id.scroller.rva == table.tableRva && id.dispatchMethodRva == table.drawRva,
              "every independently verified scroller table is admitted");
    }
    dispatch.Rax = fixture->scroller[0] = fakeWbase + fixtureTableRva;
    --dispatch.R9;
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Mismatch,
          "dispatch RECT copies must agree");
    ++dispatch.R9;
    ++dispatch.Rax;
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Mismatch,
          "dispatch RAX must contain its scroller table");
    dispatch.Rax = ++fixture->scroller[0];
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Mismatch,
          "unknown scroller table rejected");
    dispatch.Rax = --fixture->scroller[0];
    for (auto slot : {0x58, 0xC0}) {
        auto& entry = scrollerImage[(fixtureTableRva + slot) / 8];
        ++entry;
        check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Mismatch,
              "incorrect allowed-table slot rejected");
        --entry;
    }
    fixture->window[0xB8 / 8] = 0;
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::Mismatch,
          "Window must point back to the current scroller");
    fixture->window[0xB8 / 8] = fixture->address();
    const auto savedWindow = fixture->scroller[0x20 / 8];
    fixture->scroller[0x20 / 8] = ~ULONG_PTR{7};
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::ReadFailure,
          "Window backlink overflow rejected");
    fixture->scroller[0x20 / 8] = savedWindow;

    auto component = contextFor(ScrollerSite::Component, *fixture, stack);
    id = {};
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Accepted &&
          id.window.state == ScrollerLocationState::Image && id.caller.state == ScrollerLocationState::Image &&
          id.component.state == ScrollerLocationState::Image && id.componentMethod.state == ScrollerLocationState::Image &&
          id.rect == componentRect && id.componentOffset == 0x50 && id.drawFlag == 0x34 &&
          id.png == ScrollerPngObservation{},
          "component rectangle, vtable, executable slot, caller and opaque flag captured");
    ++component.R9;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Mismatch,
          "wrong component configuration pointer rejected");
    --component.R9;
    ++component.Rdi;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Mismatch,
          "out-of-layout component index rejected");
    --component.Rdi;
    ++component.Rbx;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Mismatch,
          "component slot location must match loop index");
    --component.Rbx;
    ++component.Rcx;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Mismatch,
          "component slot must contain RCX");
    --component.Rcx;
    deniedSlot = drawTable + 2;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::ReadFailure,
          "unreadable draw slot fails closed");
    deniedSlot = nullptr;
    fixture->draw = component.Rsp;
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::Accepted &&
          id.component.state == ScrollerLocationState::Unmapped && !id.component.imageBase &&
          id.componentMethod.state == ScrollerLocationState::Unmapped && !id.componentMethod.imageBase,
          "non-image component table retains no raw address");

    auto* const pages = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "allocate guarded range fixture");
    dispatch.Rbp = dispatch.R8 = dispatch.R9 = reinterpret_cast<ULONG_PTR>(pages + 4096 - sizeof(LONG));
    protection(pages + 4096, PAGE_READWRITE | PAGE_GUARD);
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::ReadFailure,
          "first coordinate readable is insufficient for the entire RECT");
    dispatch.Rbp = dispatch.R8 = dispatch.R9 = reinterpret_cast<ULONG_PTR>(pages + 4096);
    MEMORY_BASIC_INFORMATION region{};
    check(captureIdentity(dispatch, fakeWbase, ScrollerSite::Dispatch, id) == Observation::ReadFailure &&
          VirtualQuery(pages + 4096, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
          "reject guarded RECT without consuming its guard");
    check(VirtualFree(pages, 0, MEM_RELEASE) != FALSE, "release guarded fixture");
    component.Rsp = ~ULONG_PTR{7};
    check(captureIdentity(component, fakeWbase, ScrollerSite::Component, id) == Observation::ReadFailure,
          "overflowing caller slot rejected");
}

void testPngCapture()
{
    using Status = ScrollerPngStatus;
    ScrollerFixture fixture;
    std::array<ULONG_PTR, 0x200 / 8> stack{};
    auto context = contextFor(ScrollerSite::Component, fixture, stack);
    struct Component {
        ULONG_PTR table{};
        DWORD orientation{}, hotId{}, pressedId{};
        BYTE padding[0x38 - 0x14]{};
        ULONG_PTR cache{};
    } component;
    struct Cache {
        ULONG_PTR table{}, refCount{};
        std::array<ULONG_PTR, 32> slots{};
        BYTE initialized{};
    } cache;
    static_assert(offsetof(Component, cache) == 0x38);
    static_assert(offsetof(Cache, initialized) == 0x110);
    fixture.geometry[0x28 / 8] = context.Rcx = reinterpret_cast<ULONG_PTR>(&component);
    component.table = fakeWbase + 0x3D348;
    component.cache = reinterpret_cast<ULONG_PTR>(&cache);
    auto& draw = scrollerImage[(0x3D348 + 0x10) / 8];
    auto& componentGetter = scrollerImage[(0x3D348 + 0x30) / 8];
    draw = fakeWbase + 0x19290;
    componentGetter = fakeWbase + 0x1B930;
    ScrollerIdentity id{};
    const auto observe = [&](const ScrollerPngObservation& expected, const char* message) {
        check(captureIdentity(context, fakeWbase, ScrollerSite::Component, id) == Observation::Accepted &&
              id.png == expected, message);
    };

    // Cover all cache variants and both orientations, including the highest slot index.
    constexpr DWORD tables[]{0x3C458, 0x3C440, 0x3C428};
    constexpr DWORD globals[]{0x550A8, 0x550A0, 0x55098};
    for (DWORD variant = 0; variant < 3; ++variant) {
        cache.table = fakeWbase + tables[variant];
        scrollerImage[(tables[variant] + 0x08) / 8] = fakeWbase + 0x191D0;
        scrollerImage[globals[variant] / 8] = component.cache;
        observe({Status::Complete, 0, 0, 0, variant, 0, 0}, "empty PNG cache observed before initialization");
        component.orientation = 1;
        component.hotId = 5;
        component.pressedId = 4;
        cache.initialized = 1;
        cache.slots[0] = cache.slots[17] = cache.slots[31] = 1;
        observe({Status::Complete, 1, 5, 4, variant, 1, 0x80020001UL}, "PNG state and exact presence mask");
        component.orientation = component.hotId = component.pressedId = 0;
        cache.initialized = 0;
        cache.slots.fill(0);
    }

    component.orientation = 2;
    observe({Status::InvalidState}, "orientation outside 0..1 rejected");
    component.orientation = 0;
    component.hotId = 6;
    observe({Status::InvalidState}, "hot ID outside 0..5 rejected");
    component.hotId = 0;
    component.pressedId = MAXDWORD;
    observe({Status::InvalidState}, "pressed ID outside 0..5 rejected");
    component.pressedId = 0;
    ++componentGetter;
    observe({Status::InvalidComponent}, "known PNG table with wrong component getter flagged");
    --componentGetter;
    ++draw;
    observe({Status::InvalidComponent}, "known PNG table with wrong draw method flagged");
    --draw;
    deniedSlot = &componentGetter;
    observe({Status::ReadFailed}, "unreadable component getter flagged");
    deniedSlot = nullptr;

    auto& global = scrollerImage[0x55098 / 8];
    global = 0;
    observe({Status::GlobalMismatch}, "absent variant global must not validate the PNG cache");
    global = component.cache;
    deniedSlot = &global;
    observe({Status::ReadFailed}, "unreadable variant global flagged");
    deniedSlot = nullptr;
    cache.table = fakeWbase + 0x3C470;
    observe({Status::InvalidCache}, "cache table outside the concrete variants rejected");
    cache.table = fakeWbase + 0x3C428;
    auto& cacheGetter = scrollerImage[(0x3C428 + 0x08) / 8];
    ++cacheGetter;
    observe({Status::InvalidCache}, "cache getter in slot +08 must match the pinned target");
    --cacheGetter;
    cache.initialized = 2;
    observe({Status::InvalidCache}, "non-boolean initialized byte rejected");
    cache.initialized = 1;
    deniedSlot = cache.slots.data();
    observe({Status::ReadFailed}, "unreadable PNG slot range flagged");
    deniedSlot = nullptr;
    component.cache = 0;
    observe({Status::InvalidCache}, "null PNG cache rejected");
    component.cache = ~ULONG_PTR{3};
    observe({Status::ReadFailed}, "misaligned PNG cache flagged without dereference");

    component.table = reinterpret_cast<ULONG_PTR>(drawTable);
    deniedSlot = &component.cache;
    observe({}, "unknown component retains base identity without reading PNG cache");
    deniedSlot = nullptr;
    for (const auto rva : globals) scrollerImage[rva / 8] = 0;
}

void testReplayAndCounts()
{
    ScrollerFixture fixture;
    std::array<ULONG_PTR, 0x200 / 8> stack{};
    for (size_t i = 0; i < scrollerSites.size(); ++i) {
        handlerAddresses[i].store(fakeWbase + scrollerSites[i].rva);
        replaySites[i].store(true);
    }
    const auto replay = [](CONTEXT& context, ScrollerSite site) {
        EXCEPTION_RECORD record{};
        record.ExceptionCode = EXCEPTION_BREAKPOINT;
        record.ExceptionAddress = reinterpret_cast<LPVOID>(fakeWbase + scrollerSites[static_cast<size_t>(site)].rva);
        CONTEXT expected = context;
        expected.Rdx = site == ScrollerSite::Dispatch ? context.R14 : context.Rbp;
        expected.Rip = reinterpret_cast<ULONG_PTR>(record.ExceptionAddress) + 3;
        EXCEPTION_POINTERS exception{&record, &context};
        constexpr DWORD sentinel = 0xFACE1234;
        SetLastError(sentinel);
        const auto result = onBreakpoint(&exception);
        const auto lastError = GetLastError();
        check(result == EXCEPTION_CONTINUE_EXECUTION && lastError == sentinel &&
              !std::memcmp(&context, &expected, sizeof(context)),
              "replay changes only RDX and RIP and preserves LastError");
    };
    auto dispatch = contextFor(ScrollerSite::Dispatch, fixture, stack);
    replay(dispatch, ScrollerSite::Dispatch);
    check(!sampleCount, "inactive retained replay gathers nothing");
    deadline.store(tick + 10);
    replay(dispatch, ScrollerSite::Dispatch);
    replay(dispatch, ScrollerSite::Dispatch);
    auto component = contextFor(ScrollerSite::Component, fixture, stack);
    replay(component, ScrollerSite::Component);
    fixture.draw = component.Rsp;
    replay(component, ScrollerSite::Component);
    ++component.R9;
    replay(component, ScrollerSite::Component);
    --component.R9;
    component.Rsp = 0xFFFF800000000000;
    replay(component, ScrollerSite::Component);
    AcquireSRWLockExclusive(&sampleLock);
    replay(component, ScrollerSite::Component);
    ReleaseSRWLockExclusive(&sampleLock);
    for (LONG right = 1; right <= static_cast<LONG>(scrollerProbeCapacity); ++right) {
        fixture.input.right = right;
        replay(dispatch, ScrollerSite::Dispatch);
    }
    tick += 10;
    replay(dispatch, ScrollerSite::Dispatch);
    deadline.store(0);
    ScrollerProbeTrace trace;
    check(!copySamples(trace) && trace.sampleCount == scrollerProbeCapacity && trace.samples[0].calls == 2 &&
          trace.counts[0].accepted == scrollerProbeCapacity - 1 && trace.counts[0].capacityDrops == 3 &&
          trace.counts[1].accepted == 2 && trace.counts[1].unknownIdentities == 1 &&
          trace.counts[1].mismatches == 1 && trace.counts[1].readFailures == 1 && trace.counts[1].busyDrops == 1,
          "bounded numerical aggregation, distinct mismatch, read failure, unknown and contention counts");
    for (auto& active : replaySites) active.store(false);
}

struct ExecutableFixture {
    BYTE* memory{};
    std::array<BYTE*, 2> entry{};
    PatchSites sites{};

    ExecutableFixture()
    {
        memory = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(memory != nullptr, "allocate synthetic instructions");
        entry = {memory, memory + 4096};

        // Supply R14/RSI/RAX for the exact dispatch instruction window.
        constexpr BYTE firstPrologue[]{0x41, 0x56, 0x56, 0x48, 0x83, 0xEC, 0x28,
                                       0x4C, 0x8B, 0xF1, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0x06};
        constexpr BYTE firstEpilogue[]{0x10, 0x48, 0x83, 0xC4, 0x28, 0x5E, 0x41, 0x5E, 0xC3};
        auto* const first = entry[0] + sizeof(firstPrologue);
        std::memcpy(entry[0], firstPrologue, sizeof(firstPrologue));
        std::memcpy(first, scrollerSites[0].signature.data(), scrollerSites[0].signature.size());
        std::memcpy(first + scrollerSites[0].signature.size(), firstEpilogue, sizeof(firstEpilogue));

        // Supply RBP/RCX for the exact component instruction window.
        constexpr BYTE secondPrologue[]{0x57, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x20,
                                        0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xCA};
        constexpr BYTE secondEpilogue[]{0x08, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0x5B, 0x5F, 0xC3};
        auto* const second = entry[1] + sizeof(secondPrologue);
        std::memcpy(entry[1], secondPrologue, sizeof(secondPrologue));
        std::memcpy(second, scrollerSites[1].signature.data(), scrollerSites[1].signature.size());
        std::memcpy(second + scrollerSites[1].signature.size(), secondEpilogue, sizeof(secondEpilogue));
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

    void expectRestored(const ScrollerProbeTrace& trace)
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
    ScrollerProbeTrace trace;
    deniedWrite = sites[1].address;
    check(installAll(sites) == ERROR_ACCESS_DENIED && sites[0].ownsBreakpoint && !sites[1].ownsBreakpoint &&
          !finishAll(sites, trace), "second write failure rolls back first site only");
    deniedWrite = nullptr;
    fixture.expectRestored(trace);

    auto* const tail = sites[1].address + scrollerSites[1].signature.size() - 1;
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
    *sites[0].address = scrollerSites[0].signature.front();
    protection(sites[0].address, PAGE_EXECUTE_READ);
    fixture.expectRestored(trace);
}

void testActualTraps(ExecutableFixture& fixture)
{
    using Synthetic = ULONG_PTR (*)(ULONG_PTR, ULONG_PTR*);
    const auto first = reinterpret_cast<Synthetic>(fixture.entry[0]);
    const auto second = reinterpret_cast<Synthetic>(fixture.entry[1]);
    constexpr ULONG_PTR arg = 0x112233445566;
    std::array<DrawMethod, 0xC8 / 8> table{};
    table[0x10 / 8] = table[0xC0 / 8] = returnRdx;
    auto object = reinterpret_cast<ULONG_PTR>(table.data());
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    check(handler && first(arg, &object) == arg && second(arg, &object) == arg, "baseline original instructions");
    check(!installAll(fixture.sites) && *fixture.sites[0].address == 0xCC &&
          *fixture.sites[1].address == 0xCC && first(arg, &object) == arg && second(arg, &object) == arg,
          "both real INT3 instructions replay original RDX at the actual call");
    ScrollerProbeTrace trace;
    check(!finishAll(fixture.sites, trace) && first(arg, &object) == arg && second(arg, &object) == arg,
          "normal execution after two-site rollback");
    fixture.expectRestored(trace);
    check(RemoveVectoredExceptionHandler(handler) != 0, "remove test-only callback after synchronous replay");
}
}

int main()
{
    ScrollerProbeTrace trace;
    check(ScrollerIdentityProbeAbiV2(nullptr) == 2 && scrollerProbeCaptureMs == 60000,
          "scroller ABI2 and 60 second capture");
    trace.version = 1;
    check(RunScrollerIdentityProbeV2(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "legacy ABI leaves one-shot unused");
    trace.version = scrollerProbeVersion;
    trace.size = sizeof(DWORD) * 2;
    check(RunScrollerIdentityProbeV2(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "reject truncated input before mutation");
    trace.size = sizeof(trace);
    attempted = true;
    check(RunScrollerIdentityProbeV2(&trace) == ERROR_ALREADY_EXISTS, "attempt cannot re-arm");
    testCapture();
    testPngCapture();
    testReplayAndCounts();
    ExecutableFixture fixture;
    testTransaction(fixture);
    testActualTraps(fixture);
    std::puts("PASS: scroller geometry, PNG state/cache, dual INT3 replay, rollback and bounded collection");
}
