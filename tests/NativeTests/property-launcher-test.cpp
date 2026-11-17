#include "property_description_probe.hpp"
#include <cstring>

namespace {
constexpr ULONG_PTR fakeFrame = 0x181000000;
bool drift{};
unsigned reads{};

SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    if (reinterpret_cast<ULONG_PTR>(address) != fakeFrame + propertySiteRva)
        return VirtualQueryEx(process, address, region, length);
    *region = {};
    region->AllocationBase = reinterpret_cast<LPVOID>(fakeFrame);
    region->BaseAddress = const_cast<LPVOID>(address);
    region->RegionSize = propertySiteSignature.size();
    region->State = MEM_COMMIT;
    region->Type = MEM_IMAGE;
    region->Protect = PAGE_EXECUTE_READ;
    return sizeof(*region);
}

BOOL WINAPI fixtureRead(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* sizeRead)
{
    if (reinterpret_cast<ULONG_PTR>(address) != fakeFrame + propertySiteRva ||
        length != propertySiteSignature.size()) return ReadProcessMemory(process, address, output, length, sizeRead);
    auto bytes = propertySiteSignature;
    if (drift) bytes.back() ^= 1;
    std::memcpy(output, bytes.data(), length);
    *sizeRead = length;
    ++reads;
    return TRUE;
}
}

#define VirtualQueryEx fixtureQuery
#define ReadProcessMemory fixtureRead
#define PROPERTY_PROBE_LAUNCHER_TEST
#include "property_probe_launcher.cpp"
#undef ReadProcessMemory
#undef VirtualQueryEx

void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

LPVOID fixturePayload{};
DWORD fixtureCallResult{};
DWORD fixtureCall(HANDLE, ULONG_PTR, LPVOID payload, DWORD& result)
{
    fixturePayload = payload;
    if (fixtureCallResult) return fixtureCallResult;
    static_cast<PropertyProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<PropertyProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 4, "expected property, dock and production DLL paths");
    wchar_t executable[] = L"property-probe-run.exe";
    wchar_t command[] = L"--property-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(propertyProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(propertyProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(propertyProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(propertyProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(propertyProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(propertyProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    DWORD before{};
    require(probeBytes(GetCurrentProcess(), fakeFrame, before) && before == PAGE_EXECUTE_READ && reads == 1,
        "launcher independently checks the original signature");
    drift = true;
    require(!probeBytes(GetCurrentProcess(), fakeFrame, before) && reads == 2, "check the final signature byte");

    PropertyProbeTrace callTrace{};
    auto call = callPropertyProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callPropertyProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callPropertyProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callPropertyProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");
    callTrace.patch.originalProtection = callTrace.patch.finalProtection = PAGE_EXECUTE_READ;
    require(propertyProbeReplyError(callTrace, call, before) == 0, "clean reply accepted");
    callTrace.patch.finalProtection = PAGE_EXECUTE_READWRITE;
    require(propertyProbeReplyError(callTrace, call, before) == 7, "different final protection rejected");
    callTrace.patch.finalProtection = PAGE_EXECUTE_READ;
    call.payloadReleased = false;
    call.transportError = ERROR_ACCESS_DENIED;
    require(propertyProbeReplyError(callTrace, call, before) == 1, "known payload leak reports error");
    callTrace.patch.cleanupError = ERROR_WRITE_FAULT;
    require(propertyProbeReplyError(callTrace, call, before) == 7,
        "uncertain cleanup overrides simultaneous payload failure");

    using namespace propertyDescription;
    PropertyProbeTrace trace;
    trace.sampleCount = 3;
    trace.samples[0] = {{Kind::Description, 10, managerTypes[0][0], managerTypes[0][1], 0}, {1, 2, 3, 4}, 2};
    trace.samples[1] = {{Kind::List, 8, managerTypes[1][0], managerTypes[1][1], 0}, {1, 2, 3, 4}, 3};
    trace.samples[2] = {{Kind::ForeignId10, 10, 0, 0, 0}, {2, 2, 3, 4}, 4};
    trace.counts.description = 2;
    trace.counts.list = 3;
    trace.counts.foreignId10 = 4;
    require(validTrace(trace), "validate numeric classes and reconcile totals");
    trace.samples[0].identity.managerViewRva = managerTypes[1][1];
    require(!validTrace(trace), "mixed manager pair cannot pass reply validation");
    trace.samples[0].identity.managerViewRva = managerTypes[0][1];
    trace.samples[2].identity.managerRva = managerTypes[1][0];
    require(!validTrace(trace), "foreign capture does not publish an unverified manager RVA");
    trace.samples[2].identity.managerRva = 0;
    trace.samples[1].rect.right = trace.samples[1].rect.left;
    require(!validTrace(trace), "invalid geometry cannot pass reply validation");
    trace.samples[1].rect.right = 3;
    --trace.counts.description;
    require(!validTrace(trace), "totals must match sample counts");
    ++trace.counts.description;
    trace.version = 0;
    require(!validTrace(trace), "reply version must match");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own test module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    const std::wstring probePath = argv[1], dockPath = argv[2], productionPath = argv[3];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE dock = LoadLibraryW(dockPath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && dock && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), dockImage(dockPath), productionImage(productionPath);
    require(probeImage.validate(propertyProbeExport, propertyProbeAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(propertyProbeExport, propertyProbeAbiExport), "theme cannot pass probe ABI");
    require(!dockImage.validate(propertyProbeExport, propertyProbeAbiExport), "dock cannot pass property ABI");
    require(probeImage.validate(propertyProbeExport, propertyProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, propertyProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, propertyProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, propertyProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(dock);
    FreeLibrary(production);
    std::puts("PASS: explicit probe CLI, lifetime, numeric reply, and separate mapped ABIs");
}
