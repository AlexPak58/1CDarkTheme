#include "local_frame_probe.hpp"
#include <cstring>

namespace {
constexpr ULONG_PTR fakeFrame = 0x181000000;
bool driftSecond{};
std::array<unsigned, frameSites.size()> reads{};

SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    const auto current = reinterpret_cast<ULONG_PTR>(address);
    for (const auto& site : frameSites) {
        if (current != fakeFrame + site.rva) continue;
        *region = {};
        region->AllocationBase = reinterpret_cast<LPVOID>(fakeFrame);
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
    for (size_t i = 0; i < frameSites.size(); ++i) {
        const auto& site = frameSites[i];
        if (current != fakeFrame + site.rva || length != site.signature.size()) continue;
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
#define FRAME_PROBE_LAUNCHER_TEST
#include "frame_probe_launcher.cpp"
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
    static_cast<FrameProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<FrameProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 4, "expected frame, grid and production DLL paths");
    wchar_t executable[] = L"frame-probe-run.exe";
    wchar_t command[] = L"--frame-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(frameProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(frameProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(frameProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(frameProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(frameProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(frameProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    ProbeProtections before{};
    require(probeBytes(GetCurrentProcess(), fakeFrame, before) && before[0] == PAGE_EXECUTE_READ &&
            before[1] == PAGE_EXECUTE_READ && reads[0] == 1 && reads[1] == 1,
            "independent launcher reads and checks both original sites");
    driftSecond = true;
    require(!probeBytes(GetCurrentProcess(), fakeFrame, before) && reads[0] == 2 && reads[1] == 2,
            "full second signature checked independently");

    FrameProbeTrace callTrace{};
    auto call = callFrameProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callFrameProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callFrameProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callFrameProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");
    const ProbeProtections protections{PAGE_EXECUTE_READ, PAGE_EXECUTE_READ};
    for (auto& patch : callTrace.patches) patch.originalProtection = patch.finalProtection = PAGE_EXECUTE_READ;
    require(frameProbeReplyError(callTrace, call, protections) == 0, "clean reply accepted");
    callTrace.patches[1].finalProtection = PAGE_EXECUTE_READWRITE;
    require(frameProbeReplyError(callTrace, call, protections) == 7, "second-page protection must independently match");
    callTrace.patches[1].finalProtection = PAGE_EXECUTE_READ;
    call.payloadReleased = false;
    call.transportError = ERROR_ACCESS_DENIED;
    require(frameProbeReplyError(callTrace, call, protections) == 1, "known buffer leak reports error");
    callTrace.patches[1].cleanupError = ERROR_WRITE_FAULT;
    require(frameProbeReplyError(callTrace, call, protections) == 7,
            "uncertain cache cleanup overrides simultaneous buffer release failure");
    callTrace.patches[1].cleanupError = 0;
    callTrace.sampleCount = 1;
    callTrace.samples[0].calls = 1;
    require(frameProbeReplyError(callTrace, call, protections) == 7,
            "inconsistent total cannot pass even if payload release fails");
    callTrace.sampleCount = 0;
    callTrace.version = 0;
    require(frameProbeReplyError(callTrace, call, protections) == 7,
            "invalid reply overrides simultaneous buffer release failure");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    const auto base = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
    FrameLocation identity{base, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(fixtureTable) - base),
                               FrameLocationState::Image};
    require(locationModule(GetCurrentProcess(), modules, modules, identity, false), "read-only image table resolves");
    const FrameLocation method{base, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(&fixtureSetter) - base),
                               FrameLocationState::Image};
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

    const std::wstring probePath = argv[1], gridPath = argv[2], productionPath = argv[3];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE grid = LoadLibraryW(gridPath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && grid && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), gridImage(gridPath), productionImage(productionPath);
    require(probeImage.validate(frameProbeExport, frameProbeAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(frameProbeExport, frameProbeAbiExport), "theme cannot pass probe ABI");
    require(!gridImage.validate(frameProbeExport, frameProbeAbiExport), "grid cannot pass frame ABI");
    require(probeImage.validate(frameProbeExport, frameProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, frameProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, frameProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, frameProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(grid);
    FreeLibrary(production);
    std::puts("PASS: exclusive probe CLI, lifetime, image-only table resolution, separate mapped ABIs");
}
