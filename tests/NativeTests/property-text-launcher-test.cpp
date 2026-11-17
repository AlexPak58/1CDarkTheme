#include "property_text_probe.hpp"
#include <cstring>

namespace {
constexpr ULONG_PTR fakeFrame = 0x181000000;
bool drift{};
unsigned reads{};

SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    if (reinterpret_cast<ULONG_PTR>(address) != fakeFrame + propertyTextSiteRva)
        return VirtualQueryEx(process, address, region, length);
    *region = {};
    region->AllocationBase = reinterpret_cast<LPVOID>(fakeFrame);
    region->BaseAddress = const_cast<LPVOID>(address);
    region->RegionSize = propertyTextSiteSignature.size();
    region->State = MEM_COMMIT;
    region->Type = MEM_IMAGE;
    region->Protect = PAGE_EXECUTE_READ;
    return sizeof(*region);
}

BOOL WINAPI fixtureRead(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* sizeRead)
{
    if (reinterpret_cast<ULONG_PTR>(address) != fakeFrame + propertyTextSiteRva ||
        length != propertyTextSiteSignature.size()) return ReadProcessMemory(process, address, output, length, sizeRead);
    auto bytes = propertyTextSiteSignature;
    if (drift) bytes.back() ^= 1;
    std::memcpy(output, bytes.data(), length);
    *sizeRead = length;
    ++reads;
    return TRUE;
}
}

#define VirtualQueryEx fixtureQuery
#define ReadProcessMemory fixtureRead
#define PROPERTY_TEXT_PROBE_LAUNCHER_TEST
#include "property_text_launcher.cpp"
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
    static_cast<PropertyTextProbeTrace*>(payload)->elapsedMs = 123;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<PropertyTextProbeTrace*>(payload)->elapsedMs = 456;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 4, "expected upper, lower and production DLL paths");
    wchar_t executable[] = L"property-text-probe-run.exe";
    wchar_t command[] = L"--property-text-probe";
    wchar_t otherCommand[] = L"--attach";
    wchar_t pidText[] = L"0";
    wchar_t createdText[] = L"1";
    wchar_t* args[]{executable, command, pidText, createdText};
    require(propertyTextProbeCommand(1, args) == 2, "no arguments never launches 1C");
    require(propertyTextProbeCommand(4, args) == 2, "zero pid rejected");
    args[1] = otherCommand;
    require(propertyTextProbeCommand(4, args) == 2, "normal attach is not a probe command");
    args[1] = command;
    wchar_t overflow[] = L"4294967296";
    args[2] = overflow;
    require(propertyTextProbeCommand(4, args) == 2, "pid overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "process lifetime");
    const auto creation = fileTimeValue(created);
    require(propertyTextProbeToDesigner(GetCurrentProcessId(), creation + 1) == 4, "wrong lifetime rejected");
    require(propertyTextProbeToDesigner(GetCurrentProcessId(), creation) == 5, "non-1C process rejected");

    DWORD before{};
    require(probeBytes(GetCurrentProcess(), fakeFrame, before) && before == PAGE_EXECUTE_READ && reads == 1,
        "launcher independently checks the original signature");
    drift = true;
    require(!probeBytes(GetCurrentProcess(), fakeFrame, before) && reads == 2, "check the final signature byte");

    PropertyTextProbeTrace callTrace{};
    auto call = callPropertyTextProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError &&
            call.workerResult == ERROR_NOT_READY && callTrace.elapsedMs == 123,
            "worker result received and completed payload released");
    MEMORY_BASIC_INFORMATION payloadRegion{};
    require(VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_FREE,
            "released payload is not committed");
    fixtureCallResult = WAIT_TIMEOUT;
    call = callPropertyTextProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(fixturePayload, &payloadRegion, sizeof(payloadRegion)) && payloadRegion.State == MEM_COMMIT,
            "unknown worker outcome retains its payload");
    require(VirtualFreeEx(GetCurrentProcess(), fixturePayload, 0, MEM_RELEASE), "release simulated timeout fixture");
    fixtureCallResult = ERROR_ACCESS_DENIED;
    call = callPropertyTextProbe(GetCurrentProcess(), 0, callTrace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases payload");
    call = callPropertyTextProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), callTrace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult &&
            callTrace.elapsedMs == 456, "actual worker thread completes in this test process");
    callTrace.patch.originalProtection = callTrace.patch.finalProtection = PAGE_EXECUTE_READ;
    require(propertyTextProbeReplyError(callTrace, call, before) == 0, "clean reply accepted");
    callTrace.patch.finalProtection = PAGE_EXECUTE_READWRITE;
    require(propertyTextProbeReplyError(callTrace, call, before) == 7, "different final protection rejected");
    callTrace.patch.finalProtection = PAGE_EXECUTE_READ;
    call.payloadReleased = false;
    call.transportError = ERROR_ACCESS_DENIED;
    require(propertyTextProbeReplyError(callTrace, call, before) == 1, "known payload leak reports error");
    callTrace.patch.cleanupError = ERROR_WRITE_FAULT;
    require(propertyTextProbeReplyError(callTrace, call, before) == 7,
        "uncertain cleanup overrides simultaneous payload failure");

    using namespace propertyDescription;
    PropertyTextProbeTrace trace;
    trace.sampleCount = 2;
    trace.samples[0] = {{11, managerTypes[0][0], managerTypes[0][1]}, {0,0,100,30}, 2};
    trace.samples[1] = {{11, managerTypes[1][0], managerTypes[1][1]}, {-20,-10,100,30}, 3};
    trace.counts.accepted = 5;
    require(validTrace(trace), "exact ID11 logical groups reconcile");
    trace.samples[0].identity.controlId = 10;
    require(!validTrace(trace), "foreign id rejected"); trace.samples[0].identity.controlId = 11;
    trace.samples[0].identity.managerViewRva = managerTypes[1][1];
    require(!validTrace(trace), "mixed manager pair rejected"); trace.samples[0].identity.managerViewRva=managerTypes[0][1];
    trace.samples[1].identity=trace.samples[0].identity;
    require(!validTrace(trace), "duplicate logical group rejected"); trace.samples[1].identity={11,managerTypes[1][0],managerTypes[1][1]};
    trace.samples[0].calls=MAXULONGLONG;
    require(!validTrace(trace), "sample sum overflow"); trace.samples[0].calls=2;
    trace.counts.mismatches=MAXULONGLONG; trace.counts.readFailures=1;
    require(!validTrace(trace), "rejection sum overflow"); trace.counts.mismatches=1; trace.counts.readFailures=0;
    require(!validTrace(trace), "missing rejection stage"); trace.counts.rejectedStages[0]=1;
    require(validTrace(trace), "stage accounting");
    trace.counts.accepted=MAXULONGLONG; trace.counts.capacityDrops=1;
    require(!validTrace(trace), "accepted/drop overflow"); trace.counts.accepted=5; trace.counts.capacityDrops=0;
    trace.samples[0].arguments.right=0;
    require(!validTrace(trace), "empty rect rejected"); trace.samples[0].arguments.right=100;
    {
        using namespace propertyTextControl;
        PropertyTextProbeTrace diagnosticTrace;
        diagnosticTrace.diagnostics.moduleSizes = {0x1000000, 0x1000000, 0x100000};
        diagnosticTrace.counts.mismatches = 1;
        diagnosticTrace.counts.rejectedStages[static_cast<size_t>(Stage::WindowTables)] = 1;
        diagnosticTrace.diagnostics.reasons[static_cast<size_t>(Reason::WindowTable)] = 1;
        diagnosticTrace.diagnostics.used[0] = 1;
        diagnosticTrace.diagnostics.groups[0] = {{Reason::WindowTable, {Module::Other, 0}}, 1};
        require(validTrace(diagnosticTrace), "sanitized numeric rejection reconciles");
        auto& group = diagnosticTrace.diagnostics.groups[0];
        group.key.windowTable.rva = 1;
        require(!validTrace(diagnosticTrace), "Other cannot leak any address or RVA"); group.key.windowTable.rva = 0;
        group.key.windowTable = {Module::Text, 0x1000000};
        require(!validTrace(diagnosticTrace), "out of image RVA rejected");
        group.key.windowTable = {Module::Text, propertyTextControl::windowTypeRva};
        require(!validTrace(diagnosticTrace), "exact target must use reserved bucket"); group.key.windowTable = {};
        diagnosticTrace.diagnostics.overflow = 1;
        require(!validTrace(diagnosticTrace), "diagnostic overflow rejected"); diagnosticTrace.diagnostics.overflow = 0;
        diagnosticTrace.diagnostics.used[0] = 17;
        require(!validTrace(diagnosticTrace), "histogram capacity rejected"); diagnosticTrace.diagnostics.used[0] = 1;
        group.calls = MAXULONGLONG;
        require(!validTrace(diagnosticTrace), "histogram sum overflow rejected"); group.calls = 1;
        diagnosticTrace.diagnostics.reasons[1] = MAXULONGLONG;
        require(!validTrace(diagnosticTrace), "reason sum overflow rejected"); diagnosticTrace.diagnostics.reasons[1] = 1;
        diagnosticTrace.diagnostics.drops[0] = 1;
        require(!validTrace(diagnosticTrace), "histogram drop sum mismatch rejected"); diagnosticTrace.diagnostics.drops[0] = 0;
        group.key.reason = Reason::ReadWindow;
        require(!validTrace(diagnosticTrace), "histogram and first reason disagree");
    }
    {
        using namespace propertyTextControl;
        const auto singleRejection = [](Reason reason, TableToken token) {
            PropertyTextProbeTrace reply;
            reply.diagnostics.moduleSizes = {0x1000000, 0x1000000, 0x100000};
            if (reason <= Reason::Back) reply.counts.mismatches = 1;
            else reply.counts.readFailures = 1;
            reply.counts.rejectedStages[static_cast<size_t>(Stage::WindowTables)] = 1;
            reply.diagnostics.reasons[static_cast<size_t>(reason)] = 1;
            const size_t bucket = token == TableToken{Module::Text, propertyTextControl::windowTypeRva} ? 1 : 0;
            reply.diagnostics.used[bucket] = 1;
            reply.diagnostics.groups[bucket * propertyTextDiagnosticReserve] = {{reason, token}, 1};
            return reply;
        };
        const TableToken exact{Module::Text, propertyTextControl::windowTypeRva};
        require(!validTrace(singleRejection(Reason::WindowTable, exact)), "exact W cannot fail W-table predicate");
        require(!validTrace(singleRejection(Reason::ReadWindow, exact)), "failed W read cannot supply exact token");
        require(!validTrace(singleRejection(Reason::ReadWindow, {Module::Frame, 1})), "failed W read cannot supply another image token");
        require(!validTrace(singleRejection(Reason::ReadInput, exact)), "failed input cannot supply a W token");
        require(validTrace(singleRejection(Reason::ReadInput, {})) &&
                validTrace(singleRejection(Reason::ReadWindow, {})), "early read failures retain Other RVA0");
        for (const auto reason : {Reason::ReadContext, Reason::ReadAnchor}) {
            require(validTrace(singleRejection(reason, {})) &&
                    validTrace(singleRejection(reason, {Module::Frame, 1})) &&
                    validTrace(singleRejection(reason, exact)), "context and anchor reads precede table matching");
        }
        for (DWORD value = static_cast<DWORD>(Reason::ContextTable); value < static_cast<DWORD>(Reason::Count); ++value) {
            const auto reason = static_cast<Reason>(value);
            if (reason == Reason::ReadInput || reason == Reason::ReadWindow ||
                reason == Reason::ReadContext || reason == Reason::ReadAnchor) continue;
            require(!validTrace(singleRejection(reason, {})) &&
                    !validTrace(singleRejection(reason, {Module::Frame, 1})) &&
                    validTrace(singleRejection(reason, exact)), "every later predicate requires exact matched W token");
        }
        auto dropped = singleRejection(Reason::WindowTable, {});
        dropped.diagnostics.used[0] = 0;
        dropped.diagnostics.groups[0] = {};
        dropped.diagnostics.drops[0] = 1;
        require(!validTrace(dropped), "drop from empty bucket rejected");
        dropped.diagnostics.used[0] = 1;
        dropped.diagnostics.groups[0] = {{Reason::WindowTable, {}}, 1};
        dropped.counts.mismatches = 2;
        dropped.counts.rejectedStages[static_cast<size_t>(Stage::WindowTables)] = 2;
        dropped.diagnostics.reasons[static_cast<size_t>(Reason::WindowTable)] = 2;
        require(!validTrace(dropped), "drop from partially filled bucket rejected");
        dropped.diagnostics.used[0] = static_cast<DWORD>(propertyTextDiagnosticReserve);
        for (DWORD i = 0; i < propertyTextDiagnosticReserve; ++i)
            dropped.diagnostics.groups[i] = {{Reason::WindowTable, {Module::Frame, i}}, 1};
        dropped.counts.mismatches = propertyTextDiagnosticReserve + 1;
        dropped.counts.rejectedStages[static_cast<size_t>(Stage::WindowTables)] = propertyTextDiagnosticReserve + 1;
        dropped.diagnostics.reasons[static_cast<size_t>(Reason::WindowTable)] = propertyTextDiagnosticReserve + 1;
        require(validTrace(dropped), "full other bucket legitimately drops a new key");
    }
    trace.version=1; require(!validTrace(trace), "stale ABI1 rejected");

    std::vector<ProbeModule> modules;
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS, "own test module inventory");
    require(!productionThemePresent(GetCurrentProcess(), modules), "missing production theme rejected");
    const std::wstring probePath = argv[1], lowerPath = argv[2], productionPath = argv[3];
    const HMODULE probe = LoadLibraryW(probePath.c_str());
    const HMODULE lower = LoadLibraryW(lowerPath.c_str());
    const HMODULE production = LoadLibraryW(productionPath.c_str());
    require(probe && lower && production, "fixtures load only into test process");
    AttachImage probeImage(probePath), lowerImage(lowerPath), productionImage(productionPath);
    require(probeImage.validate(propertyTextProbeExport, propertyTextProbeAbiExport), "probe ABI accepted");
    require(!GetProcAddress(probe, "RunPropertyTextProbeV1") &&
            !GetProcAddress(probe, "PropertyTextProbeAbiV1") &&
            !probeImage.validate("RunPropertyTextProbeV1", "PropertyTextProbeAbiV1"), "stale helper export ABI1 rejected");
    require(probeImage.validate(propertyTextProbeExport, propertyTextProbeAbiExport), "restore ABI2 validator");
    require(!probeImage.validate(), "probe cannot pass production ABI");
    require(!productionImage.validate(propertyTextProbeExport, propertyTextProbeAbiExport), "theme cannot pass probe ABI");
    require(!lowerImage.validate(propertyTextProbeExport, propertyTextProbeAbiExport), "lower probe cannot pass upper ABI");
    require(probeImage.validate(propertyTextProbeExport, propertyTextProbeAbiExport), "restore probe validator");
    AttachModules mapped;
    require(snapshotAttachModules(GetCurrentProcessId(), probePath, mapped, propertyTextProbeDllName) == ERROR_SUCCESS &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll,
            "separate probe basename coexists with production theme");
    require(probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
            reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, propertyTextProbeExport)), "mapped probe export identity");
    require(snapshotAttachModules(GetCurrentProcessId(), L"missing-probe-path", mapped, propertyTextProbeDllName) ==
            ERROR_SUCCESS && mapped.competingDll && !mapped.ownBase, "different probe path rejected");
    require(probeModules(GetCurrentProcessId(), modules) == ERROR_SUCCESS &&
            productionThemePresent(GetCurrentProcess(), modules), "mapped production ABI verified without calling it");
    FreeLibrary(probe);
    FreeLibrary(lower);
    FreeLibrary(production);
    std::puts("PASS: explicit probe CLI, lifetime, numeric reply, and separate upper/lower mapped ABIs");
}
