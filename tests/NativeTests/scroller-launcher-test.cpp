#include "scroller_identity_probe.hpp"
#include <cstring>

namespace {
constexpr ULONG_PTR fakeWbase = 0x181000000;
bool driftSecond{};
std::array<unsigned, scrollerSites.size()> reads{};

SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    const auto current = reinterpret_cast<ULONG_PTR>(address);
    for (const auto& site : scrollerSites) {
        if (current != fakeWbase + site.rva) continue;
        *region = {};
        region->AllocationBase = reinterpret_cast<LPVOID>(fakeWbase);
        region->BaseAddress = const_cast<LPVOID>(address);
        region->RegionSize = site.signature.size();
        region->State = MEM_COMMIT;
        region->Type = MEM_IMAGE;
        region->Protect = PAGE_EXECUTE_READ;
        return sizeof(*region);
    }
    return VirtualQueryEx(process, address, region, length);
}

BOOL WINAPI fixtureRead(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* sizeRead)
{
    const auto current = reinterpret_cast<ULONG_PTR>(address);
    for (size_t i = 0; i < scrollerSites.size(); ++i) {
        const auto& site = scrollerSites[i];
        if (current != fakeWbase + site.rva || length != site.signature.size()) continue;
        auto signature = site.signature;
        if (i == 1 && driftSecond) signature.back() ^= 1;
        std::memcpy(output, signature.data(), length);
        *sizeRead = length;
        ++reads[i];
        return TRUE;
    }
    return ReadProcessMemory(process, address, output, length, sizeRead);
}
}

#define VirtualQueryEx fixtureQuery
#define ReadProcessMemory fixtureRead
#define SCROLLER_PROBE_LAUNCHER_TEST
#include "scroller_probe_launcher.cpp"
#undef ReadProcessMemory
#undef VirtualQueryEx

void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void WINAPI fixtureSetter() {}
using FixtureSlot = void (WINAPI*)();
extern "C" __declspec(dllexport) FixtureSlot const fixtureTable[9] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, fixtureSetter,
};

LPVOID fixturePayload{};
DWORD fixtureCallResult{};
DWORD fixtureCall(HANDLE, ULONG_PTR, LPVOID payload, DWORD& result)
{
    fixturePayload = payload;
    if (fixtureCallResult) return fixtureCallResult;
    static_cast<ScrollerProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<ScrollerProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 4, "expected scroller, frame and production DLL paths");
    wchar_t executable[] = L"scroller-probe-run.exe";
    wchar_t command[] = L"--scroller-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(scrollerProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(scrollerProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(scrollerProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(scrollerProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(scrollerProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(scrollerProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    ProbeProtections before{};
    require(probeBytes(GetCurrentProcess(), fakeWbase, before) && before[0] == PAGE_EXECUTE_READ &&
            before[1] == PAGE_EXECUTE_READ && reads[0] == 1 && reads[1] == 1,
            "independent launcher reads and checks both original sites");
    driftSecond = true;
    require(!probeBytes(GetCurrentProcess(), fakeWbase, before) && reads[0] == 2 && reads[1] == 2,
            "full second signature checked independently");

    ScrollerProbeTrace callTrace{};
    auto call = callScrollerProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callScrollerProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callScrollerProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callScrollerProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");
    const ProbeProtections protections{PAGE_EXECUTE_READ, PAGE_EXECUTE_READ};
    for (auto& patch : callTrace.patches) patch.originalProtection = patch.finalProtection = PAGE_EXECUTE_READ;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 0, "clean reply accepted");
    callTrace.patches[1].finalProtection = PAGE_EXECUTE_READWRITE;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 7, "second-page protection must independently match");
    callTrace.patches[1].finalProtection = PAGE_EXECUTE_READ;
    call.payloadReleased = false;
    call.transportError = ERROR_ACCESS_DENIED;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 1, "known buffer leak reports error");
    callTrace.patches[1].cleanupError = ERROR_WRITE_FAULT;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 7,
            "uncertain cache cleanup overrides simultaneous buffer release failure");
    callTrace.patches[1].cleanupError = 0;
    callTrace.sampleCount = 1;
    callTrace.samples[0].calls = 1;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 7,
            "inconsistent total cannot pass even if payload release fails");
    callTrace.sampleCount = 0;
    callTrace.version = 0;
    require(scrollerProbeReplyError(callTrace, call, protections, fakeWbase) == 7,
            "invalid reply overrides simultaneous buffer release failure");

    ScrollerProbeTrace sampleTrace{};
    sampleTrace.sampleCount = 1;
    auto& sample = sampleTrace.samples[0];
    sample.calls = 2;
    sampleTrace.counts[1].accepted = 2;
    auto& id = sample.identity;
    id.site = ScrollerSite::Component;
    id.scroller = {fakeWbase, 0x3C9A0, ScrollerLocationState::Image};
    id.dispatchMethodRva = 0x1EBC0;
    id.componentOffset = 0x40;
    id.drawFlag = 0xFF;
    id.rect = {-8, 0, 32, 128};
    id.window = {0, 0, ScrollerLocationState::Unmapped};
    id.caller = {fakeWbase, 0x1EBF1, ScrollerLocationState::Image};
    id.component = {fakeWbase, 0x3C528, ScrollerLocationState::Image};
    id.componentMethod = {fakeWbase, 0x1A460, ScrollerLocationState::Image};
    sampleTrace.counts[1].unknownIdentities = 2;
    require(validTrace(sampleTrace, fakeWbase), "wrapper and unknown Window keep sound accepted totals");
    require(!validTrace(sampleTrace, fakeWbase + 0x1000), "scroller table must belong to admitted wbase");
    id.dispatchMethodRva = 0x1E860;
    require(!validTrace(sampleTrace, fakeWbase), "scroller table and dispatch method must agree");
    id.dispatchMethodRva = 0x1EBC0;
    id.componentOffset = 0x41;
    require(!validTrace(sampleTrace, fakeWbase), "malformed component offset rejected");
    id.componentOffset = 0x40;
    id.window.imageBase = 0x11223344;
    require(!validTrace(sampleTrace, fakeWbase), "reject raw address inside unknown state");
    id.window.imageBase = 0;
    sampleTrace.counts[1].unknownIdentities = 1;
    require(!validTrace(sampleTrace, fakeWbase), "unknown totals must match sample calls");
    sampleTrace.counts[1].unknownIdentities = 2;

    using PngStatus = ScrollerPngStatus;
    id.png = {PngStatus::Complete, 1, 5, 4, 2, 1, 0x80020001UL};
    require(!validTrace(sampleTrace, fakeWbase), "unrecognized table cannot contribute PNG fields");
    id.component.rva = 0x3D348;
    id.componentMethod.rva = 0x19290;
    require(validTrace(sampleTrace, fakeWbase), "valid PNG observation admitted");
    id.png.orientation = 2;
    require(!validTrace(sampleTrace, fakeWbase), "reject out-of-range orientation");
    id.png.orientation = 1;
    id.png.hotId = 6;
    require(!validTrace(sampleTrace, fakeWbase), "reject out-of-range hot ID");
    id.png.hotId = 5;
    id.png.pressedId = 6;
    require(!validTrace(sampleTrace, fakeWbase), "reject out-of-range pressed ID");
    id.png.pressedId = 4;
    id.png.cacheVariant = 3;
    require(!validTrace(sampleTrace, fakeWbase), "reject unknown cache variant");
    id.png.cacheVariant = 2;
    id.png.cacheInitializationStarted = 2;
    require(!validTrace(sampleTrace, fakeWbase), "reject invalid initialization flag");
    id.png.cacheInitializationStarted = 1;
    id.componentMethod.rva = 0x19291;
    require(!validTrace(sampleTrace, fakeWbase), "wrong draw rejects successful PNG observation");
    id.png = {PngStatus::InvalidComponent};
    require(validTrace(sampleTrace, fakeWbase), "wrong draw can be reported as invalid component");
    id.componentMethod.rva = 0x19290;
    id.png = {PngStatus::GlobalMismatch};
    require(validTrace(sampleTrace, fakeWbase), "cache mismatch preserves accepted base identity");
    id.png.cachePresenceMask = 1;
    require(!validTrace(sampleTrace, fakeWbase), "failure must not contain PNG field values");
    id.png = {static_cast<PngStatus>(MAXDWORD)};
    require(!validTrace(sampleTrace, fakeWbase), "reject unknown PNG status");
    id.png = {};
    require(!validTrace(sampleTrace, fakeWbase), "known PNG table must have observation status");
    sampleTrace.version = 1;
    require(!validTrace(sampleTrace, fakeWbase), "legacy trace ABI rejected");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    const auto base = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
    ScrollerLocation identity{base, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(fixtureTable) - base),
                               ScrollerLocationState::Image};
    require(locationModule(GetCurrentProcess(), modules, modules, identity, false), "read-only image table resolves");
    const ScrollerLocation method{base, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(&fixtureSetter) - base),
                               ScrollerLocationState::Image};
    require(locationModule(GetCurrentProcess(), modules, modules, method, true) &&
            !locationModule(GetCurrentProcess(), modules, modules, method, false),
            "resolve verified executable code only as a method");
    const std::vector<ProbeModule> empty;
    require(!locationModule(GetCurrentProcess(), empty, modules, identity, false),
            "module absent at admission is unresolved");
    auto outside = identity;
    outside.rva = MAXDWORD;
    require(!locationModule(GetCurrentProcess(), modules, modules, outside, false), "out-of-image RVA rejected");
    outside.imageBase = reinterpret_cast<ULONG_PTR>(&outside);
    require(!locationModule(GetCurrentProcess(), modules, modules, outside, false), "no heap or stack table resolution");
    DWORD oldProtection{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), PAGE_READWRITE,
                            &oldProtection), "make fixture table writable");
    require(!locationModule(GetCurrentProcess(), modules, modules, identity, false), "writable image table rejected");
    DWORD ignored{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), oldProtection,
                            &ignored), "restore fixture protection");

    const std::wstring probePath = argv[1], framePath = argv[2], productionPath = argv[3];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE frame = LoadLibraryW(framePath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && frame && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), frameImage(framePath), productionImage(productionPath);
    require(probeImage.validate(scrollerProbeExport, scrollerProbeAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(scrollerProbeExport, scrollerProbeAbiExport), "theme cannot pass probe ABI");
    require(!frameImage.validate(scrollerProbeExport, scrollerProbeAbiExport), "frame cannot pass scroller ABI");
    require(probeImage.validate(scrollerProbeExport, scrollerProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, scrollerProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, scrollerProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, scrollerProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(frame);
    FreeLibrary(production);
    std::puts("PASS: exclusive probe CLI, lifetime, image-only table resolution, separate mapped ABIs");
}
