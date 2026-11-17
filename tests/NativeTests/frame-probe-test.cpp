#include "local_frame_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
BYTE* deniedWrite{};
BYTE* deniedRestore{};
BYTE* deniedFlush{};
LPCVOID deniedSlot{};
ULONGLONG tick = 100;

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
    if (result && address == deniedSlot) region->Protect = PAGE_NOACCESS;
    return result;
}
}

#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#define GetTickCount64 fixtureTick
#include "local_frame_probe.cpp"
#undef GetTickCount64
#undef FlushInstructionCache
#undef VirtualProtect
#undef VirtualQuery

namespace {
constexpr ULONG_PTR fakeFrame = 0x180000000;
const ULONG_PTR ownerTable[]{0x101};
const ULONG_PTR rawTable[]{0x102};
void childDrawMethod() {}
using DrawMethod = void (*)();
const DrawMethod drawTable[]{nullptr, nullptr, nullptr, childDrawMethod};
constexpr FrameProbeRect backgroundRect{-10, 15, 550, 31};
constexpr FrameProbeRect childRect{20, -2, 470, 21};

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

struct BarFixture {
    std::array<ULONG_PTR, 0x680 / 8> bar{};
    std::array<ULONG_PTR, 0xD8 / 8> owner{};
    ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(rawTable);
    ULONG_PTR draw = reinterpret_cast<ULONG_PTR>(drawTable);

    BarFixture()
    {
        bar[0] = fakeFrame + barTableRva;
        bar[0x58 / 8] = fakeFrame + barSideRva;
        bar[0x678 / 8] = reinterpret_cast<ULONG_PTR>(owner.data());
        owner[0] = reinterpret_cast<ULONG_PTR>(ownerTable);
        owner[0xD0 / 8] = address();
    }

    ULONG_PTR address() const { return reinterpret_cast<ULONG_PTR>(bar.data()); }
};

CONTEXT contextFor(FrameSite site, BarFixture& bar, std::array<ULONG_PTR, 0x200 / 8>& stack)
{
    CONTEXT context{};
    context.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
    context.EFlags = 0x203;
    if (site == FrameSite::Background) {
        stack[0x98 / 8] = fakeFrame + 0x70FB7B;
        context.Rbx = bar.address();
        context.R14 = context.Rsp + 0xF0;
        std::memcpy(&stack[0xF0 / 8], &backgroundRect, sizeof(backgroundRect));
        context.R8 = 0x00A1B2C3;
    } else {
        stack[0xA8 / 8] = fakeFrame + 0x70FC9C;
        stack[0x1A8 / 8] = fakeFrame + 0x70FBA5;
        context.Rdi = bar.address();
        context.Rbx = reinterpret_cast<ULONG_PTR>(&bar.raw);
        context.Rcx = reinterpret_cast<ULONG_PTR>(&bar.draw);
        context.R10 = reinterpret_cast<ULONG_PTR>(&childDrawMethod);
        context.Rbp = context.Rsp + 0x80;
        context.Rsi = 0x99118822;
        context.R8 = context.Rbp - 0x20;
        context.R9 = 0x34;
        std::memcpy(&stack[0x60 / 8], &childRect, sizeof(childRect));
    }
    return context;
}

void testCapture()
{
    BarFixture bar;
    std::array<ULONG_PTR, 0x200 / 8> stack{};
    auto background = contextFor(FrameSite::Background, bar, stack);
    FrameIdentity id{};
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Accepted &&
          id.owner.state == FrameLocationState::Image && id.rect == backgroundRect &&
          id.colorOrFlag == 0x00A1B2C3 && id.rawItem.state == FrameLocationState::NotRead,
          "collect exact active background geometry, color and owner module");
    background.Rdi = 1;
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Printing,
          "printing excluded and counted separately");
    background.Rdi = 0x100;
    ++background.R14;
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "wrong background rect provenance rejected");
    --background.R14;
    ++stack[0x98 / 8];
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "wrong background caller rejected");
    --stack[0x98 / 8];
    ++bar.bar[0];
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "foreign bar type rejected");
    --bar.bar[0];
    ++bar.bar[0x58 / 8];
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "wrong bar side interface rejected");
    --bar.bar[0x58 / 8];
    bar.owner[0xD0 / 8] = 0;
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "owner must point back to the current bar");
    bar.owner[0xD0 / 8] = bar.address();
    const auto savedOwner = bar.bar[0x678 / 8];
    bar.bar[0x678 / 8] = 0;
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::Mismatch,
          "missing owner rejected");
    bar.bar[0x678 / 8] = ~ULONG_PTR{7};
    check(captureIdentity(background, fakeFrame, FrameSite::Background, id) == Observation::ReadFailure,
          "owner backlink overflow rejected");
    bar.bar[0x678 / 8] = savedOwner;

    auto child = contextFor(FrameSite::Child, bar, stack);
    id = {};
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Accepted &&
          id.owner.state == FrameLocationState::Image && id.rawItem.state == FrameLocationState::Image &&
          id.drawInterface.state == FrameLocationState::Image && id.drawMethod.state == FrameLocationState::Image &&
          id.rect == childRect && id.colorOrFlag == 0x34,
          "collect child full rectangle, raw and draw vtables, executable method and flag");
    ++stack[0xA8 / 8];
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Mismatch,
          "first wrong child ancestor rejected");
    --stack[0xA8 / 8];
    ++stack[0x1A8 / 8];
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Mismatch,
          "second wrong child ancestor rejected");
    --stack[0x1A8 / 8];
    ++child.R8;
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Mismatch,
          "child rect must be RBP-20");
    --child.R8;
    child.Rbp = 8;
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::ReadFailure,
          "child rect underflow rejected");
    child.Rbp = child.Rsp + 0x80;
    child.R10 = reinterpret_cast<ULONG_PTR>(&contextFor);
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Mismatch,
          "R10 must match the current draw vtable slot");
    child.R10 = reinterpret_cast<ULONG_PTR>(&childDrawMethod);
    deniedSlot = drawTable + 3;
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::ReadFailure,
          "unreadable draw slot fails closed");
    deniedSlot = nullptr;
    bar.raw = child.Rsp;
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::Accepted &&
          id.rawItem.state == FrameLocationState::Unmapped && !id.rawItem.imageBase,
          "non-image vtable retains no raw address");
    auto* const page = static_cast<ULONG_PTR*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                                           PAGE_READWRITE));
    check(page != nullptr, "allocate guarded table fixture");
    *page = reinterpret_cast<ULONG_PTR>(rawTable);
    child.Rbx = reinterpret_cast<ULONG_PTR>(page);
    protection(page, PAGE_READWRITE | PAGE_GUARD);
    MEMORY_BASIC_INFORMATION region{};
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::ReadFailure &&
          VirtualQuery(page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
          "reject a guarded child pointer without consuming its guard");
    protection(page, PAGE_NOACCESS);
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::ReadFailure,
          "reject inaccessible child pointer");
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release guarded fixture");

    // A rectangle whose first coordinate is in range can still end outside.
    FrameProbeRect rectangle{};
    StackRange truncated{child.R8, child.R8 + sizeof(rectangle) - sizeof(LONG)};
    check(!truncated.read(child.R8, rectangle), "whole RECT must fit within the current stack range");
    child.Rsp = ~ULONG_PTR{7};
    check(captureIdentity(child, fakeFrame, FrameSite::Child, id) == Observation::ReadFailure,
          "overflowing child ancestor slot rejected");
}

void testReplayAndCounts()
{
    BarFixture bar;
    std::array<ULONG_PTR, 0x200 / 8> stack{};
    for (size_t i = 0; i < frameSites.size(); ++i) {
        handlerAddresses[i].store(fakeFrame + frameSites[i].rva);
        replaySites[i].store(true);
    }
    const auto replay = [](CONTEXT& context, FrameSite site) {
        EXCEPTION_RECORD record{};
        record.ExceptionCode = EXCEPTION_BREAKPOINT;
        record.ExceptionAddress = reinterpret_cast<LPVOID>(fakeFrame + frameSites[static_cast<size_t>(site)].rva);
        CONTEXT expected = context;
        expected.Rdx = site == FrameSite::Background ? context.R14 : context.Rsi;
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
    auto bg = contextFor(FrameSite::Background, bar, stack);
    replay(bg, FrameSite::Background);
    check(!sampleCount, "inactive retained replay gathers nothing");
    deadline.store(tick + 10);
    replay(bg, FrameSite::Background);
    replay(bg, FrameSite::Background);
    bg.Rdi = 1;
    replay(bg, FrameSite::Background);
    bg.Rdi = 0;
    auto child = contextFor(FrameSite::Child, bar, stack);
    replay(child, FrameSite::Child);
    bar.raw = child.Rsp;
    replay(child, FrameSite::Child);
    ++stack[0x1A8 / 8];
    replay(child, FrameSite::Child);
    --stack[0x1A8 / 8];
    child.Rbx = 0xFFFF800000000000;
    MEMORY_BASIC_INFORMATION ignored{};
    check(!VirtualQuery(reinterpret_cast<LPCVOID>(child.Rbx), &ignored, sizeof(ignored)),
          "real VirtualQuery failure before replay preserves sentinel");
    replay(child, FrameSite::Child);
    AcquireSRWLockExclusive(&sampleLock);
    replay(child, FrameSite::Child);
    ReleaseSRWLockExclusive(&sampleLock);
    for (DWORD color = 1; color <= frameProbeCapacity; ++color) {
        bg.R8 = color;
        replay(bg, FrameSite::Background);
    }
    tick += 10;
    replay(bg, FrameSite::Background);
    deadline.store(0);
    FrameProbeTrace trace;
    check(!copySamples(trace) && trace.sampleCount == frameProbeCapacity && trace.samples[0].calls == 2 &&
          trace.counts[0].accepted == frameProbeCapacity - 1 && trace.counts[0].printing == 1 &&
          trace.counts[0].capacityDrops == 3 && trace.counts[1].accepted == 2 &&
          trace.counts[1].unknownIdentities == 1 && trace.counts[1].mismatches == 1 &&
          trace.counts[1].readFailures == 1 && trace.counts[1].busyDrops == 1,
          "independent outcomes, bounded capacity, unknown identity and lock contention counts");
    for (auto& active : replaySites) active.store(false);
}

struct ExecutableFixture {
    BYTE* memory{};
    std::array<BYTE*, 2> entry{};
    PatchSites sites{};

    ExecutableFixture()
    {
        memory = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x320000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(memory != nullptr, "allocate synthetic instructions");
        entry = {memory, memory + 4096};
        auto* const target = memory + 256;
        constexpr BYTE returnRdx[]{0x48, 0x8B, 0xC2, 0xC3};
        std::memcpy(target, returnRdx, sizeof(returnRdx));
        constexpr BYTE bgPrologue[]{0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x4C, 0x8B, 0xF1};
        constexpr BYTE bgEpilogue[]{0x60, 0x48, 0x83, 0xC4, 0x70, 0x41, 0x5E, 0xC3};
        auto* const background = entry[0] + sizeof(bgPrologue);
        std::memcpy(entry[0], bgPrologue, sizeof(bgPrologue));
        std::memcpy(background, frameSites[0].signature.data(), frameSites[0].signature.size());
        std::memcpy(background + frameSites[0].signature.size(), bgEpilogue, sizeof(bgEpilogue));
        std::memcpy(background + 12 + 0x309E89, &target, sizeof(target));

        constexpr BYTE childPrologue[]{
            0x56, 0x55, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8D, 0x6C, 0x24, 0x50,
            0x48, 0xC7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x49, 0xBA};
        auto* const operand = entry[1] + sizeof(childPrologue);
        auto* const child = operand + sizeof(target);
        constexpr BYTE childEpilogue[]{0x48, 0x83, 0xC4, 0x38, 0x5D, 0x5E, 0xC3};
        std::memcpy(entry[1], childPrologue, sizeof(childPrologue));
        std::memcpy(operand, &target, sizeof(target));
        std::memcpy(child, frameSites[1].signature.data(), frameSites[1].signature.size());
        auto* const after = child + frameSites[1].signature.size();
        std::memset(after, 0x90, 0x0F);
        std::memcpy(after + 0x0F, childEpilogue, sizeof(childEpilogue));
        sites = {{{background, PAGE_EXECUTE_READ}, {child, PAGE_EXECUTE_READ}}};
        for (size_t i = 0; i < sites.size(); ++i) {
            protection(sites[i].address, PAGE_EXECUTE_READ);
            handlerAddresses[i].store(reinterpret_cast<ULONG_PTR>(sites[i].address));
        }
        check(FlushInstructionCache(GetCurrentProcess(), memory, 0x320000) != FALSE, "publish synthetic code");
    }

    ~ExecutableFixture()
    {
        check(VirtualFree(memory, 0, MEM_RELEASE) != FALSE, "release synthetic instructions");
    }

    void expectRestored(const FrameProbeTrace& trace)
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
    FrameProbeTrace trace;
    deniedWrite = sites[1].address;
    check(installAll(sites) == ERROR_ACCESS_DENIED && sites[0].ownsBreakpoint && !sites[1].ownsBreakpoint &&
          !finishAll(sites, trace), "second write failure rolls back first site only");
    deniedWrite = nullptr;
    fixture.expectRestored(trace);

    auto* const tail = sites[1].address + frameSites[1].signature.size() - 1;
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
    *sites[0].address = frameSites[0].signature.front();
    protection(sites[0].address, PAGE_EXECUTE_READ);
    fixture.expectRestored(trace);
}

void testActualTraps(ExecutableFixture& fixture)
{
    using Synthetic = ULONG_PTR (*)(ULONG_PTR);
    const auto first = reinterpret_cast<Synthetic>(fixture.entry[0]);
    const auto second = reinterpret_cast<Synthetic>(fixture.entry[1]);
    constexpr ULONG_PTR arg = 0x112233445566;
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    check(handler && first(arg) == arg && second(arg) == arg, "baseline original instructions");
    check(!installAll(fixture.sites) && *fixture.sites[0].address == 0xCC &&
          *fixture.sites[1].address == 0xCC && first(arg) == arg && second(arg) == arg,
          "both real INT3 instructions replay original RDX at the actual call");
    FrameProbeTrace trace;
    check(!finishAll(fixture.sites, trace) && first(arg) == arg && second(arg) == arg,
          "normal execution after two-site rollback");
    fixture.expectRestored(trace);
    check(RemoveVectoredExceptionHandler(handler) != 0, "remove test-only callback after synchronous replay");
}
}

int main()
{
    FrameProbeTrace trace;
    check(FrameProbeAbiV1(nullptr) == frameProbeVersion, "frame export ABI");
    trace.version = 0;
    check(RunLocalFrameProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "invalid ABI leaves one-shot unused");
    trace.version = frameProbeVersion;
    trace.size = sizeof(DWORD) * 2;
    check(RunLocalFrameProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted,
          "reject truncated input before mutation");
    trace.size = sizeof(trace);
    attempted = true;
    check(RunLocalFrameProbeV1(&trace) == ERROR_ALREADY_EXISTS, "attempt cannot re-arm");
    testCapture();
    testReplayAndCounts();
    ExecutableFixture fixture;
    testTransaction(fixture);
    testActualTraps(fixture);
    std::puts("PASS: strict frame geometry and object identities, dual INT3 replay, rollback and bounded collection");
}
