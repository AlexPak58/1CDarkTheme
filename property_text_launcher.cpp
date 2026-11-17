// Reuse the checked attach primitives. The production entry point is never dispatched
// by this executable: only the explicit PID + creation-time probe command is accepted.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "property_text_probe.hpp"

namespace {
constexpr wchar_t propertyTextProbeDllName[] = L"property-text-probe.dll";

struct ProbeModule {
    ULONG_PTR base{};
    DWORD size{};
    std::wstring name;
    std::wstring path;
};

DWORD probeModules(DWORD pid, std::vector<ProbeModule>& modules)
{
    modules.clear();
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    if (static_cast<HANDLE>(snapshot) == INVALID_HANDLE_VALUE) return GetLastError();
    MODULEENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Module32FirstW(snapshot, &item)) return GetLastError();
    do {
        modules.push_back({reinterpret_cast<ULONG_PTR>(item.modBaseAddr), item.modBaseSize,
                           item.szModule, item.szExePath});
    } while (Module32NextW(snapshot, &item));
    const auto error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}

bool probeBytes(HANDLE process, ULONG_PTR frame, DWORD& protection)
{
    ULONG_PTR address{};
    if (!propertyDescription::addOffset(frame, propertyTextSiteRva, address)) return false;
    MEMORY_BASIC_INFORMATION region{};
    std::array<BYTE, propertyTextSiteSignature.size()> bytes{};
    SIZE_T read{};
    if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) != frame ||
        !propertyDescription::containsBytes(region, address, bytes.size()) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(), bytes.size(), &read) ||
        read != bytes.size() || bytes != propertyTextSiteSignature) return false;
    protection = region.Protect;
    return true;
}

bool productionThemePresent(HANDLE process, const std::vector<ProbeModule>& modules)
{
    const ProbeModule* theme{};
    for (const auto& module : modules) {
        if (!pathEqual(module.name, L"tree-theme.dll")) continue;
        if (theme) return false;
        theme = &module;
    }
    if (!theme) return false;
    const Handle file(CreateFileW(theme->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(file) == INVALID_HANDLE_VALUE) return false;
    AttachImage image(theme->path);
    AttachModules mapped;
    mapped.ownBase = theme->base;
    mapped.ownSize = theme->size;
    return image.validate() && image.remoteSetter(process, mapped);
}

bool validDiagnostics(const PropertyTextProbeTrace& trace)
{
    using namespace propertyTextControl;
    const auto& diagnostic = trace.diagnostics;
    if (diagnostic.overflow || diagnostic.reasons[0]) return false;
    for (const auto size : diagnostic.moduleSizes) if (size > 0x10000000) return false;
    auto remaining = trace.counts.rejectedStages[static_cast<size_t>(propertyDescription::Stage::WindowTables)];
    auto mismatches = trace.counts.mismatches, failures = trace.counts.readFailures;
    for (size_t i = 1; i < diagnostic.reasons.size(); ++i) {
        const auto count = diagnostic.reasons[i];
        auto& outcome = i <= static_cast<size_t>(Reason::Back) ? mismatches : failures;
        if (count > remaining || count > outcome) return false;
        remaining -= count;
        outcome -= count;
    }
    if (remaining) return false;
    auto reasonRemaining = diagnostic.reasons;
    remaining = trace.counts.rejectedStages[static_cast<size_t>(propertyDescription::Stage::WindowTables)];
    for (size_t bucket = 0; bucket < 2; ++bucket) {
        if (diagnostic.used[bucket] > propertyTextDiagnosticReserve || diagnostic.drops[bucket] > remaining ||
            (diagnostic.drops[bucket] && diagnostic.used[bucket] != propertyTextDiagnosticReserve))
            return false;
        remaining -= diagnostic.drops[bucket];
        const auto start = bucket * propertyTextDiagnosticReserve;
        for (size_t i = 0; i < diagnostic.used[bucket]; ++i) {
            const auto& group = diagnostic.groups[start + i];
            const auto reason = static_cast<size_t>(group.key.reason);
            const auto module = static_cast<size_t>(group.key.windowTable.module);
            if (!reason || reason >= reasonRemaining.size() || module >= static_cast<size_t>(Module::Count) ||
                !group.calls || group.calls > remaining || group.calls > reasonRemaining[reason]) return false;
            if (module == 0 ? group.key.windowTable.rva != 0 :
                group.key.windowTable.rva >= diagnostic.moduleSizes[module - 1]) return false;
            const bool target = group.key.windowTable == TableToken{Module::Text, propertyTextControl::windowTypeRva};
            if (target != (bucket == 1)) return false;
            switch (group.key.reason) {
            case Reason::WindowTable:
                if (target) return false;
                break;
            case Reason::ReadInput:
            case Reason::ReadWindow:
                if (group.key.windowTable != TableToken{}) return false;
                break;
            case Reason::ReadContext:
            case Reason::ReadAnchor:
                // These reads precede the first W-table predicate.
                break;
            default:
                if (!target) return false;
                break;
            }
            for (size_t j = 0; j < i; ++j)
                if (diagnostic.groups[start + j].key == group.key) return false;
            remaining -= group.calls;
            reasonRemaining[reason] -= group.calls;
        }
    }
    return remaining == 0;
}

bool validTrace(const PropertyTextProbeTrace& trace)
{
    using namespace propertyDescription;
    if (trace.size != sizeof(trace) || trace.version != propertyTextProbeVersion ||
        trace.sampleCount > propertyTextProbeCapacity || !validDiagnostics(trace)) return false;
    const auto& counts = trace.counts;
    if (counts.mismatches > MAXULONGLONG - counts.readFailures) return false;
    auto rejected = counts.mismatches + counts.readFailures;
    for (const auto count : counts.rejectedStages) {
        if (count > rejected) return false;
        rejected -= count;
    }
    if (rejected || counts.accepted > MAXULONGLONG - counts.capacityDrops) return false;
    auto remaining = counts.accepted;
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto& id = sample.identity;
        if (!sample.calls || sample.calls > remaining || id.controlId != 11 ||
            !validPropertyTextArguments(sample.arguments) ||
            !((id.managerRva == managerTypes[0][0] && id.managerViewRva == managerTypes[0][1]) ||
              (id.managerRva == managerTypes[1][0] && id.managerViewRva == managerTypes[1][1]))) return false;
        for (DWORD j = 0; j < i; ++j) if (trace.samples[j].identity == id) return false;
        remaining -= sample.calls;
    }
    return remaining == 0;
}

struct ProbeCallResult {
    DWORD transportError{};
    DWORD workerResult{};
    bool received{};
    bool payloadReleased{};
};

using ProbeCall = DWORD (*)(HANDLE, ULONG_PTR, LPVOID, DWORD&);

DWORD remotePropertyTextProbeCall(HANDLE process, ULONG_PTR address, LPVOID arg, DWORD& result)
{
    const Handle worker(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), arg, 0, nullptr));
    if (!static_cast<HANDLE>(worker)) return GetLastError();
    // The diagnostic capture outlives the production launcher's 30-second wait.
    // An uncertain started worker must retain its payload and never be retried.
    if (WaitForSingleObject(worker, propertyTextProbeWaitMs) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
    return GetExitCodeThread(worker, &result) ? ERROR_SUCCESS : WAIT_TIMEOUT;
}

ProbeCallResult callPropertyTextProbe(HANDLE process, ULONG_PTR run, PropertyTextProbeTrace& trace,
                                 ProbeCall call = remotePropertyTextProbeCall)
{
    ProbeCallResult outcome;
    const auto payload = VirtualAllocEx(process, nullptr, sizeof(trace), MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
    if (!payload) { outcome.transportError = GetLastError(); return outcome; }
    SIZE_T written{};
    if (!WriteProcessMemory(process, payload, &trace, sizeof(trace), &written))
        outcome.transportError = GetLastError();
    else if (written != sizeof(trace)) outcome.transportError = ERROR_PARTIAL_COPY;
    else outcome.transportError = call(process, run, payload, outcome.workerResult);
    // A timeout leaves the worker and its payload alive. No terminate/retry/unload.
    if (outcome.transportError == WAIT_TIMEOUT) return outcome;
    SIZE_T read{};
    const BOOL readOkay = !outcome.transportError &&
        ReadProcessMemory(process, payload, &trace, sizeof(trace), &read);
    outcome.received = readOkay && read == sizeof(trace);
    if (!outcome.received && !outcome.transportError) {
        const auto error = readOkay ? ERROR_PARTIAL_COPY : GetLastError();
        outcome.transportError = error ? error : ERROR_PARTIAL_COPY;
    }
    outcome.payloadReleased = VirtualFreeEx(process, payload, 0, MEM_RELEASE) != FALSE;
    if (!outcome.payloadReleased && !outcome.transportError) outcome.transportError = GetLastError();
    return outcome;
}

int propertyTextProbeReplyError(const PropertyTextProbeTrace& trace, const ProbeCallResult& call,
                         DWORD before)
{
    if (!call.received) return attachResult(AttachStatus::Ambiguous, call.transportError, call.workerResult);
    if (!validTrace(trace)) return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_DATA, call.workerResult);
    // A byte, cache or protection ambiguity takes priority over a payload leak.
    if (trace.patch.cleanupError) return attachResult(AttachStatus::Ambiguous, trace.patch.cleanupError, call.workerResult);
    if (call.transportError || !call.payloadReleased)
        return attachResult(AttachStatus::Error, call.transportError, call.workerResult);
    if (call.workerResult || trace.patchError || trace.snapshotError)
        return attachResult(AttachStatus::Error, ERROR_SUCCESS, call.workerResult ? call.workerResult :
                            (trace.patchError ? trace.patchError : trace.snapshotError));
    if (trace.patch.originalProtection != before || trace.patch.finalProtection != before)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    return 0;
}

int propertyTextProbeToDesigner(DWORD pid, ULONGLONG creation)
{
    const Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!static_cast<HANDLE>(query)) {
        const auto error = GetLastError();
        return attachResult(error == ERROR_INVALID_PARAMETER ? AttachStatus::Stale :
                            AttachStatus::Error, error);
    }
    if (const auto error = checkProcessLifetime(query, creation)) return attachError(query, creation, error);
    std::wstring exe;
    if (const auto error = validateDesigner(query, pid, exe)) return attachError(query, creation, error);
    ThemeProcessLock lock;
    const auto lockResult = lock.acquire(pid, creation);
    if (lockResult == WAIT_ABANDONED) return attachResult(AttachStatus::Ambiguous, lockResult);
    if (lockResult == WAIT_TIMEOUT) return attachResult(AttachStatus::NotReady, lockResult);
    if (lockResult) return attachError(query, creation, lockResult);

    constexpr DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
                             PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
    const Handle process(OpenProcess(rights, FALSE, pid));
    if (!static_cast<HANDLE>(process)) return attachError(query, creation, GetLastError());
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    std::wstring reopenedExe;
    if (const auto error = validateDesigner(process, pid, reopenedExe)) return attachError(process, creation, error);
    if (!pathEqual(exe, reopenedExe.c_str())) return attachResult(AttachStatus::Stale, ERROR_PROCESS_ABORTED);
    BOOL debugged{};
    if (!CheckRemoteDebuggerPresent(process, &debugged)) return attachError(process, creation, GetLastError());
    if (debugged) return attachResult(AttachStatus::Conflict, ERROR_DEBUGGER_INACTIVE);
    const auto mainWindow = findDesignerMainWindow(pid).window;
    if (!mainWindow || IsIconic(mainWindow)) return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);

    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    if (directory.empty()) return attachResult(AttachStatus::Error, ERROR_PATH_NOT_FOUND);
    const auto dll = (directory / propertyTextProbeDllName).wstring();
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules, propertyTextProbeDllName))
        return attachError(process, creation, error);
    // Any previous load, even after an interrupted caller, prohibits automatic rearming.
    if (modules.competingDll || modules.ownBase)
        return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (modules.frame.empty() || modules.graphics.empty())
        return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    std::vector<ProbeModule> beforeModules;
    if (const auto error = probeModules(pid, beforeModules)) return attachError(process, creation, error);
    const ProbeModule* frameModule{};
    const ProbeModule* textModule{};
    for (const auto& module : beforeModules) {
        if (pathEqual(module.name, L"txtedui.dll")) {
            if (textModule) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
            textModule = &module;
        }
        if (!pathEqual(module.name, L"wbase83.dll")) continue;
        if (frameModule) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
        frameModule = &module;
    }
    if (!frameModule || !textModule)
        return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    if (!matchesSha256(modules.frame, frameHash) || !matchesSha256(modules.graphics, graphicsHash) ||
        !matchesSha256(frameModule->path, wbaseHash) || !matchesSha256(textModule->path, propertyTextModuleHash))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (!productionThemePresent(process, beforeModules))
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);
    const auto frame = frameModule->base;
    DWORD beforeProtection{};
    if (!probeBytes(process, frame, beforeProtection))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate(propertyTextProbeExport, propertyTextProbeAbiExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    ULONG_PTR unusedSetter{};
    const auto loadError = loadTheme(process, pid, dll, unusedSetter, propertyTextProbeExport);
    if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
        return attachResult(AttachStatus::Ambiguous, loadError);
    if (loadError) return attachError(process, creation, loadError);
    if (const auto error = snapshotAttachModules(pid, dll, modules, propertyTextProbeDllName))
        return attachResult(AttachStatus::Ambiguous, error);
    if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    const auto run = image.remoteSetter(process, modules);
    if (!run) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    if (!CheckRemoteDebuggerPresent(process, &debugged))
        return attachError(process, creation, GetLastError());
    if (debugged) return attachResult(AttachStatus::Conflict, ERROR_DEBUGGER_INACTIVE);
    const auto readyWindow = findDesignerMainWindow(pid).window;
    if (readyWindow != mainWindow || IsIconic(readyWindow))
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);
    DWORD justBeforeProtection{};
    if (!probeBytes(process, frame, justBeforeProtection) || justBeforeProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);

    PropertyTextProbeTrace trace{};
    const auto call = callPropertyTextProbe(process, run, trace);
    const auto result = call.workerResult;
    if (call.transportError == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, WAIT_TIMEOUT);
    DWORD afterProtection{};
    if (checkProcessLifetime(process, creation) || !probeBytes(process, frame, afterProtection) ||
        afterProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, result);
    if (const auto error = propertyTextProbeReplyError(trace, call, beforeProtection)) return error;
    const std::array<const wchar_t*, 3> diagnosticNames{L"frame.dll", L"txtedui.dll", L"wbase83.dll"};
    for (size_t i = 0; i < diagnosticNames.size(); ++i) {
        const auto found = std::find_if(beforeModules.begin(), beforeModules.end(), [&](const auto& module) {
            return pathEqual(module.name, diagnosticNames[i]);
        });
        if (found == beforeModules.end() || trace.diagnostics.moduleSizes[i] != found->size)
            return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_DATA);
    }
    const auto& counts = trace.counts;
    std::printf("{\"version\":2,\"status\":\"%s\",\"cleanupVerified\":true,\"payloadReleased\":true,"
                "\"pid\":%lu,\"creation\":\"%llu\",\"elapsedMs\":%lu,\"siteRva\":%lu,"
                "\"accepted\":%llu,\"mismatches\":%llu,\"readFailures\":%llu,"
                "\"capacityDrops\":%llu,\"busyDrops\":%llu,\"samples\":[",
                trace.sampleCount ? "captured" : "no_samples", pid, creation, trace.elapsedMs, propertyTextSiteRva,
                counts.accepted, counts.mismatches, counts.readFailures, counts.capacityDrops, counts.busyDrops);
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        if (i) std::putchar(',');
        std::printf("{\"controlId\":%lu,\"managerRva\":%lu,\"managerViewRva\":%lu,\"calls\":%llu,"
                    "\"rect\":[%ld,%ld,%ld,%ld]}", sample.identity.controlId, sample.identity.managerRva,
                    sample.identity.managerViewRva, sample.calls, sample.arguments.left, sample.arguments.top,
                    sample.arguments.right, sample.arguments.bottom);
    }
    std::printf("],\"rejectedStages\":[");
    for (size_t i = 0; i < counts.rejectedStages.size(); ++i) {
        if (i) std::putchar(',');
        std::printf("%llu", counts.rejectedStages[i]);
    }
    std::printf("],\"windowDiagnostic\":{\"moduleSizes\":[%lu,%lu,%lu],\"overflow\":%lu,\"reasons\":[",
                trace.diagnostics.moduleSizes[0], trace.diagnostics.moduleSizes[1],
                trace.diagnostics.moduleSizes[2], trace.diagnostics.overflow);
    for (size_t i = 0; i < trace.diagnostics.reasons.size(); ++i) {
        if (i) std::putchar(',');
        std::printf("%llu", trace.diagnostics.reasons[i]);
    }
    std::printf("],\"drops\":[%llu,%llu],\"groups\":[", trace.diagnostics.drops[0], trace.diagnostics.drops[1]);
    bool comma = false;
    for (size_t bucket = 0; bucket < 2; ++bucket) {
        for (size_t i = 0; i < trace.diagnostics.used[bucket]; ++i) {
            const auto& group = trace.diagnostics.groups[bucket * propertyTextDiagnosticReserve + i];
            if (comma) std::putchar(',');
            comma = true;
            std::printf("{\"reason\":%lu,\"module\":%lu,\"rva\":%lu,\"calls\":%llu}",
                        static_cast<DWORD>(group.key.reason), static_cast<DWORD>(group.key.windowTable.module),
                        group.key.windowTable.rva, group.calls);
        }
    }
    std::puts("]}}");
    return 0;
}

int propertyTextProbeCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    if (argc != 4 || std::wstring_view(argv[1]) != L"--property-text-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return propertyTextProbeToDesigner(pid, creation);
}
} // namespace

#ifndef PROPERTY_TEXT_PROBE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return propertyTextProbeCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
