#include "property_description_probe.hpp"

namespace {
BYTE* denyWritable{};
BYTE* denyRestore{};
BYTE* denyFlush{};

BOOL WINAPI testProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous)
{
    if ((address == denyWritable && protection == PAGE_EXECUTE_READWRITE) ||
        (address == denyRestore && protection == PAGE_EXECUTE_READ)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return VirtualProtect(address, size, protection, previous);
}

BOOL WINAPI testFlush(HANDLE process, LPCVOID address, SIZE_T size)
{
    if (address == denyFlush) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushInstructionCache(process, address, size);
}
}

#define VirtualProtect testProtect
#define FlushInstructionCache testFlush
#include "property_description_probe.cpp"
#undef FlushInstructionCache
#undef VirtualProtect

namespace {

void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

constexpr ULONG_PTR fakeFrame = 0x181000000;

template <typename T>
void put(void* base, size_t offset, T value)
{
    std::memcpy(static_cast<BYTE*>(base) + offset, &value, sizeof(value));
}

ULONG_PTR address(const void* pointer, size_t offset = 0)
{
    return reinterpret_cast<ULONG_PTR>(pointer) + offset;
}

// Substitute image provenance and the two vtable slots. The live guard reader
// still reads all object fields, with stack geometry read from the current thread.
struct ImageFixtureReader : GuardedReader {
    using GuardedReader::read;
    bool read(ULONG_PTR base, ULONG_PTR offset, ULONG_PTR& value) const
    {
        if (base == fakeFrame + windowDrawTypeRva && offset == 0x120) {
            value = fakeFrame + 0x5D3230;
            return true;
        }
        if (base == fakeFrame + idTypeRva && offset == 0x18) {
            value = fakeFrame + 0x7C1670;
            return true;
        }
        return GuardedReader::read(base, offset, value);
    }

    bool imageTable(ULONG_PTR frame, ULONG_PTR table, SIZE_T length) const
    {
        return frame == fakeFrame && table >= frame + 0xAB0000 &&
            table < frame + 0xB40000 && length <= 0x128;
    }
};

struct Objects {
    alignas(8) BYTE window[0x3C8]{}, owner[0x978]{}, site[0x138]{}, form[0x4A8]{},
        shellBlock[0x90]{}, manager[0x40]{};
    ULONG_PTR draw{address(window, 0x160)};
    ULONG_PTR shell{address(shellBlock, 0x18)};

    Objects()
    {
        put(window, 0, fakeFrame + windowTypeRva);
        put(window, 0x160, fakeFrame + windowDrawTypeRva);
        put(window, 0x160 + 0x260, address(owner));
        put(window, 0x160 + 0x68, address(owner, 0x5C8));
        put(owner, 0x970, address(window));
        put(owner, 0x2E0, fakeFrame + ownerTypeRva);
        put(owner, 0x5C8, DWORD{0x00D2D9E6});
        put(owner, 0x5E8, ULONG_PTR{1});
        put(owner, 0x2A8, address(site));
        put(owner, 0x528, address(site, 0x18));
        put(site, 0, fakeFrame + siteTypeRva);
        put(site, 8, address(form));
        put(site, 0x18, fakeFrame + idTypeRva);
        setId(10);
        put(form, 0, fakeFrame + formTypeRva);
        put(form, 0x28, fakeFrame + formViewTypeRva);
        put(form, 0x4A0, shell + 0x10);
        put(shellBlock, 0x18, fakeFrame + shellTypeRva);
        put(shellBlock, 0x28, fakeFrame + outerTypeRva);
        put(shellBlock, 0x18 + 0x28, address(form) - 0x18);
        put(shellBlock, 0x18 + 0x30, address(form, 0x28));
        put(shellBlock, 0x18 + 0x70, address(manager));
        put(shellBlock, 0x18 + 0x18, address(manager, 8));
        put(manager, 0x28, address(shellBlock));
        put(manager, 0x30, shell);
        put(manager, 0x38, address(form, 0x28));
        setManager(0, 0);
    }

    void setId(DWORD value) { put(site, 0x18 + 0x114, value); }

    void setManager(size_t root, size_t view)
    {
        put(manager, 0, fakeFrame + managerTypes[root][0]);
        put(manager, 8, fakeFrame + managerTypes[view][1]);
    }
};

void testCapture()
{
    Objects objects;
    alignas(16) BYTE stack[0x280]{};
    CONTEXT ctx{};
    ctx.Rdi = objects.draw;
    ctx.Rbp = 1;
    ctx.Rsp = address(stack);
    ctx.R14 = address(stack, 0x270);
    ctx.R8 = 0x12345678FFFFFFFC; // Only the low 32 bits are an argument.
    ctx.R9 = 10;
    put(stack, 0xA8, fakeFrame + returnRvas[0]);
    put(stack, 0x188, fakeFrame + returnRvas[1]);
    const PropertyRect expected{-4, 10, 300, 60};
    put(stack, 0x270, expected);
    put(stack, 0x120, expected);
    put(stack, 0x20, LONG{304});
    put(stack, 0x28, LONG{50});
    put(stack, 0x30, DWORD{0x00F00021});
    Identity id{};
    PropertyRect actual{};
    Stage stage{};
    const auto capture = [&] { return captureIdentity(ctx, fakeFrame, id, actual, ImageFixtureReader{}, &stage); };
    for (size_t pair = 0; pair < managerTypes.size(); ++pair) {
        objects.setManager(pair, pair);
        objects.setId(10);
        require(capture() == Outcome::Accepted && id.kind == Kind::Description && id.controlId == 10 &&
            id.managerRva == managerTypes[pair][0] && id.managerViewRva == managerTypes[pair][1] &&
            id.rawColor == 0x00D2D9E6 && actual == expected, "description backed by a matching manager pair");
        objects.setId(8);
        require(capture() == Outcome::Accepted && id.kind == Kind::List, "classify property list separately");
    }
    ctx.R14 = address(stack, 0x120);
    require(capture() == Outcome::Accepted && actual == expected, "type-13 uses inset paint rectangle");
    objects.setId(10);
    put(objects.form, 0, fakeFrame + formTypeRva + 0x08);
    require(capture() == Outcome::Accepted && id.kind == Kind::ForeignId10 && !id.managerRva &&
        !id.managerViewRva, "recognize an explicit unrelated family with the same control ID");
    put(objects.form, 0, ULONG_PTR{});
    require(capture() == Outcome::ReadFailure, "null table is not a foreign identity");
    put(objects.form, 0, fakeFrame + formTypeRva);
    objects.setManager(0, 1);
    require(capture() == Outcome::Mismatch, "mixed manager pair rejected");
    require(stage == Stage::ManagerTables, "identify rejected manager tables");
    objects.setManager(0, 0);
    put(objects.manager, 0x38, address(objects.form));
    require(capture() == Outcome::Mismatch, "wrong manager backlink rejected");
    put(objects.manager, 0x38, address(objects.form, 0x28));
    put(objects.owner, 0x970, objects.draw);
    require(capture() == Outcome::Mismatch, "wrong window backlink rejected");
    require(stage == Stage::Owner, "identify rejected owner backlink");
    put(objects.owner, 0x970, address(objects.window));
    put(objects.owner, 0x528, address(objects.site));
    require(capture() == Outcome::Mismatch, "wrong ID interface relationship rejected");
    put(objects.owner, 0x528, address(objects.site, 0x18));
    put(objects.owner, 0x5E8, ULONG_PTR{});
    require(capture() == Outcome::ReadFailure, "missing opaque brush rejected without dereferencing it");
    put(objects.owner, 0x5E8, ULONG_PTR{1});
    put(stack, 0x188, fakeFrame + returnRvas[1] + 1);
    require(capture() == Outcome::Mismatch, "wrong outer return site rejected");
    require(stage == Stage::Returns, "identify rejected call chain before object reads");
    put(stack, 0x188, fakeFrame + returnRvas[1]);
    put(stack, 0x30, DWORD{0xCC0020});
    require(capture() == Outcome::Mismatch, "wrong ROP rejected");
    require(stage == Stage::RectValues, "identify rejected geometry or ROP");
    put(stack, 0x30, DWORD{0xF00021});
    put(stack, 0x20, LONG{303});
    require(capture() == Outcome::Mismatch, "RECT and PatBlt arguments must agree");
    put(stack, 0x120, PropertyRect{(std::numeric_limits<LONG>::min)(), 0, MAXLONG, 1});
    require(capture() == Outcome::Mismatch, "widened geometry rejects overflow");
    ctx.Rsp = (std::numeric_limits<ULONG_PTR>::max)() - 1;
    require(capture() == Outcome::ReadFailure, "stack-offset overflow rejected");

    auto* guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    require(guard != nullptr, "allocate guard fixture");
    DWORD value{};
    require(!GuardedReader{}.read(address(guard), 0, value), "guard page refused before reading");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(guard, &region, sizeof(region)) && (region.Protect & PAGE_GUARD),
        "guard was not consumed");
    require(VirtualFree(guard, 0, MEM_RELEASE), "free guarded fixture");
}

void testCountsAndContext()
{
    const auto site = fakeFrame + propertySiteRva;
    handlerAddress.store(site);
    replaySite.store(true);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = site + 1;
    ctx.Rax = ctx.Rcx = ctx.Rdx = ctx.R11 = ctx.R15 = 0x12345678;
    ctx.EFlags = 0x202;
    ctx.Rbp = 0x76543210;
    auto expected = ctx;
    expected.Rdx = ctx.Rbp;
    expected.Rip = site + 3;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = reinterpret_cast<void*>(site + 1);
    EXCEPTION_POINTERS exception{&record, &ctx};
    require(onBreakpoint(&exception) == EXCEPTION_CONTINUE_SEARCH && ctx.Rip == site + 1,
        "ignore an unrelated trap");
    record.ExceptionAddress = reinterpret_cast<void*>(site);
    SetLastError(0x4321);
    deadline.store(GetTickCount64() + captureMs);
    require(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION && GetLastError() == 0x4321 &&
        !std::memcmp(&ctx, &expected, sizeof(ctx)) && counts.readFailures == 1,
        "replay modifies only RDX and RIP and preserves LastError after failed collection");
    require(counts.rejectedStages[static_cast<size_t>(Stage::Stack)] == 1,
        "record the numeric rejection stage without exposing memory");
    AcquireSRWLockExclusive(&sampleLock);
    require(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION, "replay when sample lock is busy");
    ReleaseSRWLockExclusive(&sampleLock);
    require(busyDrops.load() == 1, "contention counted");
    deadline.store(1);
    require(onBreakpoint(&exception) == EXCEPTION_CONTINUE_EXECUTION && counts.readFailures == 1,
        "expired deadline keeps replay without collecting");
    deadline.store(0);
    replaySite.store(false);

    const Identity id{Kind::Description, 10, managerTypes[0][0], managerTypes[0][1], 0};
    for (LONG index = 0; index < static_cast<LONG>(propertyProbeCapacity); ++index)
        recordSample(id, {index, 0, index + 1, 1});
    recordSample(id, {0, 0, 1, 1});
    recordSample(id, {-1, 0, 0, 1});
    PropertyProbeTrace trace;
    require(!copySamples(trace) && trace.sampleCount == propertyProbeCapacity &&
        trace.counts.description == propertyProbeCapacity + 1 && trace.counts.capacityDrops == 1 &&
        trace.counts.busyDrops == 1 && trace.samples[0].calls == 2, "aggregate within the fixed capacity");
}

ULONG_PTR WINAPI returnSecond(void*, ULONG_PTR value) { return value; }

void testPatch()
{
    constexpr SIZE_T length = 0x800000;
    auto* code = static_cast<BYTE*>(VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(code != nullptr, "allocate exact-instruction fixture");
    constexpr BYTE prologue[]{0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xEA, 0x4C, 0x8B, 0xD9, 0x33, 0xD2};
    constexpr BYTE epilogue[]{0x00, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3};
    auto* patch = code + sizeof(prologue);
    std::memcpy(code, prologue, sizeof(prologue));
    std::memcpy(patch, propertySiteSignature.data(), propertySiteSignature.size());
    std::memcpy(patch + propertySiteSignature.size(), epilogue, sizeof(epilogue));
    const auto protect = [&](DWORD value) {
        DWORD previous{};
        require(VirtualProtect(code, 4096, value, &previous), "protect synthetic code page");
    };
    protect(PAGE_EXECUTE_READ);
    require(FlushInstructionCache(GetCurrentProcess(), code, 4096), "publish fixture code");
    using Synthetic = ULONG_PTR (WINAPI*)(void*, ULONG_PTR);
    using Slot = decltype(&returnSecond);
    const auto method = reinterpret_cast<Synthetic>(code);
    std::array<Slot, 0x1F0 / 8> table{};
    table[0x1E8 / 8] = returnSecond;
    require(method(table.data(), 0x2345) == 0x2345, "unpatched exact window executes");
    const auto handler = AddVectoredExceptionHandler(1, onBreakpoint);
    require(handler != nullptr, "register test replay");
    handlerAddress.store(address(patch));

    PatchSite site{patch, PAGE_EXECUTE_READ};
    PropertyProbePatch report;
    denyWritable = patch;
    require(install(site) == ERROR_ACCESS_DENIED && !replaySite.load() && !site.ownsBreakpoint &&
        !finishPatch(site, report), "failed write leaves original instructions");
    denyWritable = nullptr;
    protect(PAGE_EXECUTE_READWRITE);
    patch[propertySiteSignature.size() - 1] ^= 1;
    protect(PAGE_EXECUTE_READ);
    require(install(site) == ERROR_INVALID_DATA && !site.ownsBreakpoint &&
        finishPatch(site, report) == ERROR_INVALID_DATA, "last-byte drift blocks installation");
    protect(PAGE_EXECUTE_READWRITE);
    patch[propertySiteSignature.size() - 1] ^= 1;
    protect(PAGE_EXECUTE_READ);

    require(!install(site) && *patch == 0xCC, "install only first byte");
    SetLastError(0x1234);
    require(method(table.data(), 0x4567) == 0x4567 && GetLastError() == 0x1234,
        "real breakpoint replays MOV RDX,RBP before original indirect call");
    denyRestore = patch;
    require(finishPatch(site, report) == ERROR_ACCESS_DENIED && report.finalProtection == PAGE_EXECUTE_READWRITE,
        "surface failed protection restoration");
    denyRestore = nullptr;
    require(!finishPatch(site, report) && report.finalProtection == PAGE_EXECUTE_READ && signatureMatches(patch),
        "restore protection and exact signature after fixture recovery");
    require(!install(site), "arm before failed cache flush");
    denyFlush = patch;
    require(finishPatch(site, report) == ERROR_WRITE_FAULT && site.needsFlush,
        "failed flush remains observable even when bytes match");
    denyFlush = nullptr;
    require(!finishPatch(site, report) && !site.needsFlush, "test fixture retries flush");
    require(!install(site), "arm before CAS conflict");
    protect(PAGE_EXECUTE_READWRITE);
    *patch = 0x90;
    protect(PAGE_EXECUTE_READ);
    require(finishPatch(site, report) == ERROR_INVALID_DATA && *patch == 0x90,
        "cleanup preserves a competing opcode");
    protect(PAGE_EXECUTE_READWRITE);
    *patch = propertySiteSignature.front();
    protect(PAGE_EXECUTE_READ);
    require(!finishPatch(site, report) && signatureMatches(patch) &&
        FlushInstructionCache(GetCurrentProcess(), code, 4096) && method(table.data(), 0x7654) == 0x7654,
        "unpatched execution after restoring fixture");
    require(RemoveVectoredExceptionHandler(handler) && VirtualFree(code, 0, MEM_RELEASE), "release test replay");
}

} // namespace

int main()
{
    PropertyProbeTrace trace;
    require(PropertyDescriptionProbeAbiV2(nullptr) == propertyProbeVersion, "diagnostic ABI");
    trace.version = 0;
    require(RunPropertyDescriptionProbeV2(&trace) == ERROR_INVALID_PARAMETER && !attempted,
        "wrong ABI rejected before one-shot");
    trace.version = propertyProbeVersion;
    trace.size = sizeof(DWORD) * 2;
    require(RunPropertyDescriptionProbeV2(&trace) == ERROR_INVALID_PARAMETER && !attempted,
        "short payload rejected before one-shot");
    trace.size = sizeof(trace);
    attempted = true;
    require(RunPropertyDescriptionProbeV2(&trace) == ERROR_ALREADY_EXISTS, "reject rearming");
    testCapture();
    testCountsAndContext();
    testPatch();
    std::puts("PASS: property identity, numeric capture, exact INT3 replay and rollback");
}
