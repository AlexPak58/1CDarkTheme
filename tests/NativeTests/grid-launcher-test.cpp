#define GRID_PROBE_LAUNCHER_TEST
#include "grid_probe_launcher.cpp"

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
    static_cast<GridProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<GridProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 4, "expected grid, counter and production DLL paths");
    wchar_t executable[] = L"grid-probe-run.exe";
    wchar_t command[] = L"--grid-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(gridProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(gridProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(gridProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(gridProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(gridProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(gridProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    GridProbeTrace callTrace{};
    auto call = callGridProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callGridProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callGridProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callGridProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");
    callTrace.originalProtection = callTrace.finalProtection = PAGE_EXECUTE_READ;
    require(gridProbeReplyError(callTrace, call, PAGE_EXECUTE_READ) == 0, "clean reply accepted");
    call.payloadReleased = false;
    call.transportError = ERROR_ACCESS_DENIED;
    require(gridProbeReplyError(callTrace, call, PAGE_EXECUTE_READ) == 1, "known buffer leak reports error");
    callTrace.cleanupError = ERROR_WRITE_FAULT;
    require(gridProbeReplyError(callTrace, call, PAGE_EXECUTE_READ) == 7,
            "uncertain cache cleanup overrides simultaneous buffer release failure");
    callTrace.cleanupError = 0;
    callTrace.sampleCount = 1;
    callTrace.samples[0].calls = 1;
    require(gridProbeReplyError(callTrace, call, PAGE_EXECUTE_READ) == 7,
            "inconsistent total cannot pass even if payload release fails");
    callTrace.sampleCount = 0;
    callTrace.version = 0;
    require(gridProbeReplyError(callTrace, call, PAGE_EXECUTE_READ) == 7,
            "invalid reply overrides simultaneous buffer release failure");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    const auto base = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
    GridTableIdentity identity{base, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(fixtureTable) - base),
                               GridTableState::ImageTable};
    require(tableModule(GetCurrentProcess(), modules, modules, identity), "read-only image table resolves");
    const std::vector<ProbeModule> empty;
    require(!tableModule(GetCurrentProcess(), empty, modules, identity), "module absent at admission is unresolved");
    auto outside = identity;
    outside.rva = MAXDWORD;
    require(!tableModule(GetCurrentProcess(), modules, modules, outside), "out-of-image RVA rejected");
    outside.imageBase = reinterpret_cast<ULONG_PTR>(&outside);
    require(!tableModule(GetCurrentProcess(), modules, modules, outside), "no heap or stack table resolution");
    DWORD oldProtection{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), PAGE_READWRITE,
                            &oldProtection), "make fixture table writable");
    require(!tableModule(GetCurrentProcess(), modules, modules, identity), "writable image table rejected");
    DWORD ignored{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), oldProtection,
                            &ignored), "restore fixture protection");

    const std::wstring probePath = argv[1], counterPath = argv[2], productionPath = argv[3];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE counter = LoadLibraryW(counterPath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && counter && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), counterImage(counterPath), productionImage(productionPath);
    require(probeImage.validate(gridProbeExport, gridProbeAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(gridProbeExport, gridProbeAbiExport), "theme cannot pass probe ABI");
    require(!counterImage.validate(gridProbeExport, gridProbeAbiExport), "counter cannot pass grid ABI");
    require(probeImage.validate(gridProbeExport, gridProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, gridProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, gridProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, gridProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(counter);
    FreeLibrary(production);
    std::puts("PASS: exclusive probe CLI, lifetime, image-only table resolution, separate mapped ABIs");
}
