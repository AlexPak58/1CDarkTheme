#define COUNTER_PROBE_LAUNCHER_TEST
#include "counter_probe_launcher.cpp"

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
    static_cast<CounterProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<CounterProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 3, "expected probe and production DLL paths");
    wchar_t executable[] = L"counter-probe-run.exe";
    wchar_t command[] = L"--counter-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(counterProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(counterProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(counterProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(counterProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(counterProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(counterProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    CounterProbeTrace callTrace{};
    auto call = callCounterProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callCounterProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callCounterProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callCounterProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    CounterProbeSample sample{};
    sample.table = reinterpret_cast<ULONG_PTR>(fixtureTable);
    const auto resolution = resolveProbeSample(GetCurrentProcess(), modules, sample);
    require(resolution.tableModule && resolution.setterModule &&
            resolution.setter == reinterpret_cast<ULONG_PTR>(&fixtureSetter), "readonly image slot resolves");
    const auto tableModule = resolution.tableModule;
    require(!containingModule(modules, tableModule->base + tableModule->size), "exclusive module end");
    require(!containingModule(modules, tableModule->base + tableModule->size - 1, 2), "range cannot span image end");
    sample.table += 1;
    require(!resolveProbeSample(GetCurrentProcess(), modules, sample).tableModule, "misaligned table rejected");
    sample.table = ~ULONG_PTR{0} - 0x10;
    require(!resolveProbeSample(GetCurrentProcess(), modules, sample).tableModule, "address overflow rejected");
    sample.table = reinterpret_cast<ULONG_PTR>(&sample);
    require(!resolveProbeSample(GetCurrentProcess(), modules, sample).tableModule, "stack object is not followed");
    DWORD oldProtection{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), PAGE_READWRITE,
                            &oldProtection), "make fixture table writable");
    sample.table = reinterpret_cast<ULONG_PTR>(fixtureTable);
    require(!resolveProbeSample(GetCurrentProcess(), modules, sample).tableModule, "writable image data rejected");
    DWORD ignored{};
    require(VirtualProtect(const_cast<FixtureSlot*>(fixtureTable), sizeof(fixtureTable), oldProtection,
                            &ignored), "restore fixture protection");

    const std::wstring probePath = argv[1], productionPath = argv[2];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), productionImage(productionPath);
    require(probeImage.validate(counterProbeExport, counterProbeAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(counterProbeExport, counterProbeAbiExport), "theme cannot pass probe ABI");
    require(probeImage.validate(counterProbeExport, counterProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, counterProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, counterProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, counterProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(production);
    std::puts("PASS: exclusive probe CLI, lifetime, image-only slot resolution, separate mapped ABIs");
}
