#include "property_text_probe.hpp"

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
#include "property_text_probe.cpp"
#undef FlushInstructionCache
#undef VirtualProtect

namespace {

void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

constexpr ULONG_PTR fakeFrame = 0x181000000;
constexpr ULONG_PTR fakeText = 0x182000000;
bool invalidIdGetter{};

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
            value = fakeFrame + 0x7C1670 + (invalidIdGetter ? 1 : 0);
            return true;
        }
        if (base == fakeFrame + propertyPaint::proxySiteTypeRva && offset == 0x18) {
            value = fakeFrame + 0x8A7A80;
            return true;
        }
        if (base == fakeFrame + propertyPaint::proxyIdTypeRva && offset == 0x18) {
            value = fakeFrame + 0x8A7AA0;
            return true;
        }
        if (base == fakeText + propertyTextControl::windowTypeRva && offset == 0x78) {
            value = fakeText + 0xDC0E5; return true;
        }
        return GuardedReader::read(base, offset, value);
    }

    bool imageTable(ULONG_PTR frame, ULONG_PTR table, SIZE_T length) const
    {
        return (frame == fakeText && table >= frame && table - frame < 0x100000) ||
            (frame == fakeFrame && table >= frame + 0xA60000 &&
            table < frame + 0xB40000 && length <= 0x128);
    }
};

struct Objects {
    alignas(8) BYTE window[0x428]{}, owner[0x978]{}, site[0x138]{}, form[0x4A8]{},
        shellBlock[0x90]{}, manager[0x40]{}, head[0x30]{}, node[0x30]{}, proxy[0x238]{};
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
        put(form, 8, address(head));
        put(head, 8, address(node));
        put(head, 0x19, BYTE{1});
        put(node, 0, address(head));
        put(node, 0x10, address(head));
        put(node, 0x28, address(site) - 8);
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

    void setId(DWORD value) {
        put(site, 0x18 + 0x114, value);
        put(node, 0x20, value);
    }

    void attachProxy() {
        put(owner, 0x2A0, fakeFrame + propertyPaint::ownerNativeTypeRva);
        put(owner, 0x2A8, address(proxy));
        put(owner, 0x528, address(proxy, 8));
        put(proxy, 0, fakeFrame + propertyPaint::proxySiteTypeRva);
        put(proxy, 8, fakeFrame + propertyPaint::proxyIdTypeRva);
        put(proxy, 0x40, fakeFrame + propertyPaint::proxyHostTypeRva);
        put(proxy, 0x1C0, address(owner, 0x2A0));
        put(proxy, 0x1D0, address(owner, 0x2E0));
        put(proxy, 0x228, address(site));
        put(proxy, 0x230, address(site, 0x18));
        put(site, 0x10, address(proxy, 0x40));
    }

    void setManager(size_t root, size_t view)
    {
        put(manager, 0, fakeFrame + managerTypes[root][0]);
        put(manager, 8, fakeFrame + managerTypes[view][1]);
    }
};

struct TextObjects : Objects {
    alignas(8) BYTE textWindow[0x198]{}, root[0x758]{};
    TextObjects() {
        setId(11);
        put(textWindow, 0, fakeText + propertyTextControl::windowTypeRva);
        put(textWindow, 8, address(root, 0x100));
        put(textWindow, 0x190, fakeText + propertyTextControl::contextTypeRva);
        put(root, 0, fakeFrame + 0xA65200);
        put(root, 0x100, fakeFrame + 0xA64B78);
        put(root, 0x600, fakeFrame + 0xA64200);
        put(root, 0x750, address(textWindow, 0x190));
        put(root, 8, address(site));
        put(root, 0x288, address(site, 0x18));
        put(root, 0x290, address(site, 0x20));
        put(root, 0x298, address(site, 0xA0));
    }
};
void testCapture() {
    TextObjects objects;
    alignas(16) BYTE stack[0xB0]{};
    CONTEXT ctx{};
    ctx.Rsp = address(stack); ctx.Rbp = ctx.Rsp + 0x49;
    ctx.Rsi = address(objects.textWindow); ctx.Rax = fakeText + propertyTextControl::windowTypeRva;
    ctx.Rdx = 0xF;
    put(objects.textWindow, 0x140, address(stack, 0x50));
    put(stack, 0x50, ULONG_PTR{1});
    put(stack, 0x58, LONG{-20}); put(stack, 0x5C, LONG{0});
    put(stack, 0x60, LONG{120}); put(stack, 0x64, LONG{30});
    propertyTextControl::Identity id{};
    PropertyTextArguments args{};
    Stage stage{};
    const auto capture = [&] { return captureIdentity(ctx, fakeFrame, id, args, ImageFixtureReader{}, &stage, fakeText); };
    for (size_t pair = 0; pair < managerTypes.size(); ++pair) {
        objects.setManager(pair, pair);
        require(capture() == Outcome::Accepted && id.controlId == 11 && id.managerRva == managerTypes[pair][0], "ID11 exact family");
    }
    invalidIdGetter = true;
    require(capture() == Outcome::Mismatch && stage == Stage::ControlId,
        "wrong ID getter rejected despite exact Site and control tables");
    invalidIdGetter = false;
    require(capture() == Outcome::Accepted, "exact ID getter restored");
    for (DWORD rejected : {7u, 8u, 10u, 12u}) {
        objects.setId(rejected); require(capture() == Outcome::Mismatch, "other ID rejected");
    }
    objects.setId(11);
    propertyDescription::Identity oldId{};
    bool brush{};
    require(propertyDescription::inspect(fakeFrame, objects.draw, oldId, ImageFixtureReader{}) == Outcome::Mismatch,
        "old description selector still rejects11");
    require(propertyPaint::inspectPaint(fakeFrame, address(objects.window), address(objects.owner, 0x5C8), oldId, brush, ImageFixtureReader{}) == Outcome::Mismatch,
        "old paint selector still rejects11");
    require(propertyPaint::registeredControl(ImageFixtureReader{}, fakeFrame, address(objects.site), 11) == Outcome::Mismatch,
        "old registry default rejects11");
    objects.setId(10);
    put(objects.form, 0, fakeFrame + formTypeRva + 8);
    require(inspectFamily(ImageFixtureReader{}, fakeFrame, address(objects.form), oldId) == Outcome::Mismatch,
        "unset old identity cannot become foreign");
    oldId.controlId = 10;
    require(inspectFamily(ImageFixtureReader{}, fakeFrame, address(objects.form), oldId) == Outcome::Accepted && oldId.kind == Kind::ForeignId10,
        "old foreign10 behavior preserved");
    objects.setId(11);
    require(capture() == Outcome::Mismatch, "foreign11 rejected");
    put(objects.form, 0, fakeFrame + formTypeRva);
    put(objects.root, 0x750, address(objects.textWindow)); require(capture() == Outcome::Mismatch, "root backlink");
    put(objects.root, 0x750, address(objects.textWindow, 0x190));
    put(objects.root, 0x290, address(objects.site)); require(capture() == Outcome::Mismatch, "site view linkage");
    put(objects.root, 0x290, address(objects.site, 0x20));
    objects.setManager(0, 1); require(capture() == Outcome::Mismatch, "mixed managers"); objects.setManager(0, 0);
    put(objects.manager, 0x38, address(objects.form)); require(capture() == Outcome::Mismatch, "manager links");
    put(objects.manager, 0x38, address(objects.form, 0x28));
    put(objects.node, 0x28, address(objects.site)); require(capture() == Outcome::Mismatch, "registry exact wrapper");
    put(objects.node, 0x28, address(objects.site) - 8);
    put(objects.node, 0x19, BYTE{1}); require(capture() == Outcome::Mismatch, "wrong sentinel");
    put(objects.node, 0x19, BYTE{0});
    put(objects.node, 0, address(objects.node)); require(capture() == Outcome::Mismatch, "registry cycle");
    put(objects.node, 0, address(objects.head));
    put(objects.head, 8, address(objects.head)); require(capture() == Outcome::Mismatch, "missing registry");
    put(objects.head, 8, address(objects.node));
    alignas(8) BYTE nodes[65][0x30]{};
    for (size_t i = 0; i < 65; ++i) {
        put(nodes[i], 0, i == 64 ? address(objects.head) : address(nodes[i+1]));
        put(nodes[i], 0x20, DWORD{11}); put(nodes[i], 0x28, address(objects.site)-8);
    }
    put(objects.head, 8, address(nodes)); require(capture() == Outcome::Mismatch, "64 node limit");
    put(objects.head, 8, address(objects.node));
    ++ctx.Rbp; require(capture() == Outcome::Mismatch, "RBP relation"); --ctx.Rbp;
    put(objects.textWindow, 0x140, address(stack, 0x58)); require(capture() == Outcome::Mismatch, "context pointer before rect");
    put(objects.textWindow, 0x140, address(stack, 0x50));
    put(stack, 0x60, LONG{-20}); require(capture() == Outcome::Mismatch, "empty signed rect"); put(stack, 0x60, LONG{120});
    ctx.Rsp = MAXULONGLONG - 0x20; require(capture() == Outcome::ReadFailure, "stack overflow");
    ctx.Rsp = address(stack)+1; require(capture() == Outcome::ReadFailure, "unaligned stack");
    ctx.Rsp = address(stack); ctx.Rsi = 1; require(capture() == Outcome::ReadFailure, "unreadable window");
}
struct WindowDiagnosticReader : GuardedReader {
    using GuardedReader::read;
    mutable unsigned reads{}, images{};
    unsigned failRead{}, failImage{};
    std::array<ULONG_PTR, 8> values{
        fakeText + propertyTextControl::windowTypeRva, fakeText + propertyTextControl::contextTypeRva,
        0x100000100, fakeText + 0xDC0E5, fakeFrame + 0xA65200,
        fakeFrame + 0xA64B78, fakeFrame + 0xA64200, 0x200000190};
    bool read(ULONG_PTR, ULONG_PTR, ULONG_PTR& value) const {
        ++reads;
        if (reads == failRead || reads > values.size()) return false;
        value = values[reads - 1];
        return true;
    }
    bool imageTable(ULONG_PTR, ULONG_PTR, SIZE_T) const { return ++images != failImage; }
};

void testWindowDiagnostics() {
    using namespace propertyTextControl;
    const ImageRanges ranges{{{fakeFrame, 0x1000000}, {fakeText, 0x1000000}, {0x183000000, 0x100000}}};
    const auto check = [&](WindowDiagnosticReader& reader, Outcome expected, Reason reason, unsigned reads) {
        propertyTextControl::Identity identity{};
        Diagnostic diagnostic{};
        Stage stage{};
        const auto outcome = inspectText(fakeFrame, fakeText, 0x200000000, identity, reader, &stage, &diagnostic, ranges);
        require(outcome == expected && diagnostic.reason == reason && reader.reads == reads,
                "first reason and original read sequence");
        if (reason != Reason::WindowTable && reason != Reason::ReadWindow)
            require(diagnostic.windowTable == TableToken{Module::Text, propertyTextControl::windowTypeRva}, "bounded exact W token");
    };
    for (size_t i = 0; i < 8; ++i) {
        WindowDiagnosticReader reader;
        reader.values[i] = i == 2 ? 0x100 : 1;
        check(reader, Outcome::Mismatch, static_cast<Reason>(i + 1), i < 3 ? 3 : 8);
    }
    constexpr Reason readReasons[]{Reason::ReadWindow, Reason::ReadContext, Reason::ReadAnchor,
        Reason::ReadMethod, Reason::ReadRoot, Reason::ReadRootView, Reason::ReadRootDraw, Reason::ReadBack};
    for (unsigned i = 1; i <= 8; ++i) {
        WindowDiagnosticReader reader;
        reader.failRead = i;
        check(reader, Outcome::ReadFailure, readReasons[i - 1], i);
    }
    constexpr Reason imageReasons[]{Reason::ImageWindow, Reason::ImageContext, Reason::ImageRoot,
        Reason::ImageRootView, Reason::ImageRootDraw};
    for (unsigned i = 1; i <= 5; ++i) {
        WindowDiagnosticReader reader;
        reader.failImage = i;
        check(reader, Outcome::ReadFailure, imageReasons[i - 1], i < 3 ? 3 : 8);
    }
    propertyTextControl::Identity identity{}; Diagnostic diagnostic{}; Stage stage{};
    require(inspectText(0, fakeText, 1, identity, WindowDiagnosticReader{}, &stage, &diagnostic, ranges) ==
        Outcome::ReadFailure && diagnostic.reason == Reason::ReadInput && diagnostic.windowTable == TableToken{},
        "input failure has sanitized zero token");
    WindowDiagnosticReader reader;
    reader.values[0] = 0xDEADBEEF12345678;
    require(inspectText(fakeFrame, fakeText, 0x200000000, identity, reader, &stage, &diagnostic, ranges) ==
        Outcome::Mismatch && diagnostic.windowTable == TableToken{}, "unknown absolute pointer becomes Other RVA0");
    require(tableToken(0x183100000, ranges) == TableToken{} &&
        tableToken(fakeFrame - 1, ranges) == TableToken{} &&
        tableToken(fakeText + 0x1000000, ranges) == TableToken{Module::Wbase, 0} &&
        tableToken(fakeFrame + 5, ranges) == TableToken{Module::Frame, 5} &&
        tableToken(0x183000010, ranges) == TableToken{Module::Wbase, 16}, "all image bounds and identities");
    diagnostics = {};
    for (DWORD i = 0; i < propertyTextDiagnosticReserve; ++i)
        recordDiagnostic({Reason::WindowTable, {Module::Frame, i}});
    recordDiagnostic({Reason::WindowTable, {Module::Frame, 100}});
    require(diagnostics.used[0] == propertyTextDiagnosticReserve && diagnostics.drops[0] == 1 &&
        diagnostics.used[1] == 0, "other histogram bounded independently");
    for (DWORD i = 1; i <= propertyTextDiagnosticReserve; ++i)
        recordDiagnostic({static_cast<Reason>(i), {Module::Text, propertyTextControl::windowTypeRva}});
    recordDiagnostic({Reason::ReadRootView, {Module::Text, propertyTextControl::windowTypeRva}});
    recordDiagnostic({Reason::WindowTable, {Module::Frame, 0}});
    recordDiagnostic({Reason::WindowTable, {Module::Text, propertyTextControl::windowTypeRva}});
    require(diagnostics.used[1] == propertyTextDiagnosticReserve && diagnostics.drops[1] == 1 &&
        diagnostics.groups[0].calls == 2 && diagnostics.groups[propertyTextDiagnosticReserve].calls == 2,
        "exact target reserve cannot starve; full buffers still update existing keys");
    diagnostics.groups[0].calls = MAXULONGLONG;
    recordDiagnostic({Reason::WindowTable, {Module::Frame, 0}});
    require(diagnostics.overflow == 1 && diagnostics.groups[0].calls == MAXULONGLONG,
        "group overflow flagged without wrapping");
    diagnostics = {};
    for (DWORD i = 0; i < propertyTextDiagnosticReserve; ++i)
        recordDiagnostic({Reason::WindowTable, {Module::Frame, i}});
    diagnostics.drops[0] = MAXULONGLONG;
    recordDiagnostic({Reason::WindowTable, {Module::Frame, 100}});
    require(diagnostics.overflow == 1 && diagnostics.drops[0] == MAXULONGLONG, "drop overflow flagged");
    propertyTextControl::ImageRange actualImage{};
    require(diagnosticImage(GetModuleHandleW(nullptr), actualImage) && actualImage.size &&
        !diagnosticImage(nullptr, actualImage), "PE image size read outside collector");
    diagnostics = {};
    diagnostics.reasons[1] = MAXULONGLONG;
    recordDiagnostic({Reason::WindowTable, {}});
    require(diagnostics.overflow == 1 && diagnostics.reasons[1] == MAXULONGLONG, "reason overflow flagged");
    diagnostics = {};
}

void testReplayAndPatch() {
    auto* code = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
    require(code != nullptr, "allocate code");
    // push rsi; mov rsi,rcx; [exact MOV RCX,RSI site]; mov rax,rcx; pop rsi; ret
    const BYTE prefix[]{0x56,0x48,0x8B,0xF1};
    std::memcpy(code, prefix, sizeof(prefix));
    auto* patch = code + sizeof(prefix);
    std::memcpy(patch, propertyTextSiteSignature.data(), propertyTextSiteSignature.size());
    // Skip the signature tail in the fixture using its call target. RAX is the table.
    // Instead execute a jump immediately after MOV for the actual INT3 replay fixture.
    // Keep patch rollback signature exact by restoring the tail before patch checks.
    DWORD previous{};
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous), "protect code");
    PatchSite site{patch,PAGE_EXECUTE_READ}; PropertyTextProbePatch report;
    denyWritable = patch;
    require(install(site)==ERROR_ACCESS_DENIED && !site.ownsBreakpoint, "write denial"); denyWritable=nullptr;
    require(!finishPatch(site, report), "unmodified rollback");
    require(!install(site) && *patch==0xCC, "install");
    const auto handler=AddVectoredExceptionHandler(1,onBreakpoint); require(handler!=nullptr,"handler");
    handlerAddress.store(address(patch)); deadline.store(0);
    CONTEXT ctx{}; ctx.Rsi=0x123456789; ctx.Rip=address(patch)+1; ctx.EFlags=0xAD7;
    ctx.Rax=0xFEED; ctx.Rdx=0xF; auto expected=ctx; expected.Rcx=ctx.Rsi; expected.Rip=address(patch)+3;
    EXCEPTION_RECORD rec{}; rec.ExceptionCode=EXCEPTION_BREAKPOINT; rec.ExceptionAddress=patch;
    EXCEPTION_POINTERS ep{&rec,&ctx}; SetLastError(0x4321);
    require(onBreakpoint(&ep)==EXCEPTION_CONTINUE_EXECUTION && !std::memcmp(&ctx,&expected,sizeof(ctx)) && GetLastError()==0x4321,"MOV replay exact CONTEXT/LastError");
    denyRestore=patch; require(finishPatch(site,report)==ERROR_ACCESS_DENIED,"restore failure surfaced"); denyRestore=nullptr;
    require(!finishPatch(site,report) && signatureMatches(patch),"restore exact bytes/protection");
    require(onBreakpoint(&ep)==EXCEPTION_CONTINUE_EXECUTION,"queued trap after restore");
    // Real INT3: callable fixture preserves RSI, uses replayed RCX, skips unused signature tail.
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READWRITE,&previous),"write fixture tail");
    const BYTE tail[]{0x48,0x8B,0xC1,0x5E,0xC3};
    std::memcpy(patch+3,tail,sizeof(tail)); *patch=0xCC;
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous) && FlushInstructionCache(GetCurrentProcess(),code,4096),"publish real trap");
    using Method=ULONG_PTR(WINAPI*)(ULONG_PTR);
    SetLastError(0x5678);
    require(reinterpret_cast<Method>(code)(0xABCDEF)==0xABCDEF && GetLastError()==0x5678,"actual INT3 MOV replay");
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READWRITE,&previous),"restore fixture signature");
    std::memcpy(patch,propertyTextSiteSignature.data(),propertyTextSiteSignature.size());
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous),"protect restored fixture");
    require(!install(site),"arm flush test"); denyFlush=patch;
    require(finishPatch(site,report)==ERROR_WRITE_FAULT && site.needsFlush,"flush failure"); denyFlush=nullptr;
    require(!finishPatch(site,report),"flush recovery");
    require(!install(site),"arm conflict");
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READWRITE,&previous),"write conflict"); *patch=0x90;
    require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous),"protect conflict");
    require(finishPatch(site,report)==ERROR_INVALID_DATA && *patch==0x90,"CAS preserves competitor");
    require(RemoveVectoredExceptionHandler(handler) && VirtualFree(code,0,MEM_RELEASE),"release fixture");
}

void testAggregationAndContention() {
    sampleCount = 0;
    samples = {};
    counts = {};
    busyDrops.store(0);
    const propertyTextControl::Identity identity{11, managerTypes[0][0], managerTypes[0][1]};
    const PropertyTextArguments firstRect{0, 0, 10, 10}, resizedRect{-20, -10, 120, 30};
    recordSample(identity, firstRect);
    recordSample(identity, resizedRect);
    require(sampleCount == 1 && samples[0].calls == 2 && counts.accepted == 2 &&
        samples[0].arguments == resizedRect, "resizing retains one logical group and updates last RECT");

    // Exercise the storage boundary directly with distinct synthetic logical keys.
    // The production selector admits only the two known manager pairs.
    for (DWORD index = 1; index < propertyTextProbeCapacity; ++index)
        recordSample({11, managerTypes[0][0] + index, managerTypes[0][1]}, firstRect);
    require(sampleCount == propertyTextProbeCapacity && counts.accepted == propertyTextProbeCapacity + 1 &&
        counts.capacityDrops == 0, "fill every bounded sample slot");
    recordSample({11, MAXDWORD, MAXDWORD}, firstRect);
    require(sampleCount == propertyTextProbeCapacity && counts.capacityDrops == 1 &&
        counts.accepted == propertyTextProbeCapacity + 1, "new group at capacity is counted only as a drop");
    recordSample(identity, firstRect);
    require(sampleCount == propertyTextProbeCapacity && samples[0].calls == 3 &&
        samples[0].arguments == firstRect && counts.accepted == propertyTextProbeCapacity + 2 &&
        counts.capacityDrops == 1, "existing group increments and updates RECT when table is full");

    const auto accepted = counts.accepted, mismatches = counts.mismatches, failures = counts.readFailures;
    CONTEXT ctx{};
    AcquireSRWLockExclusive(&sampleLock);
    recordCall(ctx, fakeFrame);
    ReleaseSRWLockExclusive(&sampleLock);
    require(busyDrops.load() == 1 && counts.accepted == accepted && counts.mismatches == mismatches &&
        counts.readFailures == failures, "busy sample lock increments only busyDrops without inspecting context");
    PropertyTextProbeTrace trace;
    require(copySamples(trace) == ERROR_SUCCESS && trace.sampleCount == propertyTextProbeCapacity &&
        trace.counts.busyDrops == 1 && trace.counts.capacityDrops == 1 &&
        trace.counts.accepted == propertyTextProbeCapacity + 2 && trace.samples[0].calls == 3,
        "snapshot preserves full table and separate accepted capacity and contention accounting");
}
} // namespace
int main() {
    PropertyTextProbeTrace trace;
    require(PropertyTextProbeAbiV2(nullptr)==2,"ABI2"); trace.version=0;
    require(RunPropertyTextProbeV2(&trace)==ERROR_INVALID_PARAMETER && !attempted,"bad ABI before oneshot");
    trace.version=1;
    require(RunPropertyTextProbeV2(&trace)==ERROR_INVALID_PARAMETER && !attempted,"stale ABI1 before oneshot");
    trace.version=2; trace.size=8;
    require(RunPropertyTextProbeV2(&trace)==ERROR_INVALID_PARAMETER && !attempted,"short payload");
    trace.size=sizeof(trace); attempted=true;
    require(RunPropertyTextProbeV2(&trace)==ERROR_ALREADY_EXISTS,"oneshot");
    testCapture(); testWindowDiagnostics(); testReplayAndPatch(); testAggregationAndContention();
    std::puts("PASS: ID11 selector, stack guards, exact MOV replay and rollback");
}
