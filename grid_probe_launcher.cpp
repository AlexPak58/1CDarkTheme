// Reuse the checked attach primitives. The production entry point is never dispatched
// by this executable: only the explicit PID + creation-time probe command is accepted.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "grid_identity_probe.hpp"

namespace {
constexpr wchar_t gridProbeDllName[] = L"grid-identity-probe.dll";

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

bool probeImageRegion(HANDLE process, ULONG_PTR address, SIZE_T length, ULONG_PTR base,
                      bool code, MEMORY_BASIC_INFORMATION& region)
{
    if (!length || !VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address),
                                  &region, sizeof(region))) return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) != base || address < start ||
        address - start >= region.RegionSize || length > region.RegionSize - (address - start) ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    // Tables must be immutable image data. Do not follow a captured heap pointer.
    const auto protection = region.Protect & 0xff;
    return code ? protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE :
                  protection == PAGE_READONLY;
}

bool probeBytes(HANDLE process, ULONG_PTR frame, DWORD& protection)
{
    if (!frame || frame > (std::numeric_limits<ULONG_PTR>::max)() - gridProbeRva) return false;
    MEMORY_BASIC_INFORMATION region{};
    const auto address = frame + gridProbeRva;
    std::array<BYTE, gridProbeSignature.size()> bytes{};
    SIZE_T read{};
    if (!probeImageRegion(process, address, bytes.size(), frame, true, region) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(),
                           bytes.size(), &read) || read != bytes.size() ||
        bytes != gridProbeSignature) return false;
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

void printProbeJsonString(const std::wstring& text)
{
    std::putchar('"');
    for (const auto ch : text) {
        if (ch >= 0x20 && ch <= 0x7e && ch != L'"' && ch != L'\\')
            std::putchar(static_cast<int>(ch));
        else std::printf("\\u%04x", static_cast<unsigned int>(ch));
    }
    std::putchar('"');
}

void printProbeAddress(const ProbeModule* module, ULONG_PTR address)
{
    if (!module) { std::fputs("null", stdout); return; }
    std::fputs("{\"module\":", stdout);
    printProbeJsonString(module->name);
    std::printf(",\"rva\":\"0x%llx\"}", address - module->base);
}

const ProbeModule* tableModule(HANDLE process, const std::vector<ProbeModule>& before,
                              const std::vector<ProbeModule>& after, const GridTableIdentity& identity)
{
    if (identity.state != GridTableState::ImageTable ||
        identity.rva > (std::numeric_limits<ULONG_PTR>::max)() - identity.imageBase) return nullptr;
    for (const auto& current : after) {
        if (current.base != identity.imageBase || identity.rva >= current.size ||
            sizeof(ULONG_PTR) > current.size - identity.rva) continue;
        for (const auto& original : before) {
            if (original.base != current.base || original.size != current.size ||
                !pathEqual(original.path, current.path.c_str())) continue;
            MEMORY_BASIC_INFORMATION region{};
            if (probeImageRegion(process, current.base + identity.rva, sizeof(ULONG_PTR),
                                 current.base, false, region)) return &current;
        }
    }
    return nullptr;
}

bool printTable(HANDLE process, const std::vector<ProbeModule>& before,
                const std::vector<ProbeModule>& after, const GridTableIdentity& identity)
{
    const auto module = tableModule(process, before, after, identity);
    std::printf("{\"state\":%lu,\"location\":", static_cast<DWORD>(identity.state));
    printProbeAddress(module, identity.imageBase + identity.rva);
    std::putchar('}');
    return identity.state == GridTableState::NotRead || identity.state == GridTableState::Null || module;
}

bool validTrace(const GridProbeTrace& trace)
{
    if (trace.size != sizeof(trace) || trace.version != gridProbeVersion ||
        trace.sampleCount > gridProbeCapacity) return false;
    auto remaining = trace.totalCalls;
    const auto debit = [&remaining](ULONGLONG calls) {
        if (calls > remaining) return false;
        remaining -= calls;
        return true;
    };
    const auto validTable = [](const GridTableIdentity& table) {
        if (table.state == GridTableState::ImageTable)
            return table.imageBase && !(table.rva % alignof(ULONG_PTR));
        return table.state < GridTableState::ImageTable && !table.imageBase && !table.rva;
    };
    if (!debit(trace.droppedCalls) || !debit(trace.otherCallerCalls) || !debit(trace.stackReadFailures))
        return false;
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto& id = sample.identity;
        if (!sample.calls || !debit(sample.calls) || id.kind > GridRootKind::Aggregated ||
            id.alias > GridRowsAlias::ProviderPlusOffset || !validTable(id.root) ||
            !validTable(id.provider) || !validTable(id.rows) || !validTable(id.site))
            return false;
    }
    return !remaining;
}

struct ProbeCallResult {
    DWORD transportError{};
    DWORD workerResult{};
    bool received{};
    bool payloadReleased{};
};

using ProbeCall = DWORD (*)(HANDLE, ULONG_PTR, LPVOID, DWORD&);

ProbeCallResult callGridProbe(HANDLE process, ULONG_PTR run, GridProbeTrace& trace,
                                 ProbeCall call = remoteCall)
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

int gridProbeReplyError(const GridProbeTrace& trace, const ProbeCallResult& call,
                           DWORD beforeProtection)
{
    if (!call.received) return attachResult(AttachStatus::Ambiguous, call.transportError, call.workerResult);
    // An uncertain instruction-cache/patch cleanup takes priority over a payload leak.
    if (!validTrace(trace) || trace.cleanupError)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_DATA, call.workerResult);
    if (call.transportError || !call.payloadReleased)
        return attachResult(AttachStatus::Error, call.transportError, call.workerResult);
    if (call.workerResult || trace.patchError)
        return attachResult(AttachStatus::Error, ERROR_SUCCESS,
                            call.workerResult ? call.workerResult : trace.patchError);
    if (trace.originalProtection != beforeProtection || trace.finalProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    return 0;
}

int gridProbeToDesigner(DWORD pid, ULONGLONG creation)
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
    const auto dll = (directory / gridProbeDllName).wstring();
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules, gridProbeDllName))
        return attachError(process, creation, error);
    // Any previous load, even after an interrupted caller, prohibits automatic rearming.
    if (modules.competingDll || modules.ownBase)
        return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (modules.frame.empty() || modules.graphics.empty() || modules.frontend.empty())
        return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    if (!matchesSha256(modules.frame, frameHash) || !matchesSha256(modules.graphics, graphicsHash) ||
        !matchesSha256(modules.frontend, listFrontendHash))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    std::vector<ProbeModule> beforeModules;
    if (const auto error = probeModules(pid, beforeModules)) return attachError(process, creation, error);
    if (!productionThemePresent(process, beforeModules))
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);
    const auto frame = remoteModule(pid, modules.frame);
    DWORD beforeProtection{};
    if (!probeBytes(process, frame, beforeProtection))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate(gridProbeExport, gridProbeAbiExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    ULONG_PTR unusedSetter{};
    const auto loadError = loadTheme(process, pid, dll, unusedSetter, gridProbeExport);
    if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
        return attachResult(AttachStatus::Ambiguous, loadError);
    if (loadError) return attachError(process, creation, loadError);
    if (const auto error = snapshotAttachModules(pid, dll, modules, gridProbeDllName))
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

    GridProbeTrace trace{};
    const auto call = callGridProbe(process, run, trace);
    const auto result = call.workerResult;
    if (call.transportError == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, WAIT_TIMEOUT);
    DWORD afterProtection{};
    if (checkProcessLifetime(process, creation) || !probeBytes(process, frame, afterProtection) ||
        afterProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, result);
    if (const auto error = gridProbeReplyError(trace, call, beforeProtection)) return error;
    std::vector<ProbeModule> afterModules;
    if (const auto error = probeModules(pid, afterModules))
        return attachResult(AttachStatus::Error, error);
    std::printf("{\"status\":\"%s\",\"cleanupVerified\":true,\"payloadReleased\":true,\"pid\":%lu,"
                "\"creation\":\"%llu\",\"elapsedMs\":%lu,\"totalCalls\":%llu,"
                "\"droppedCalls\":%llu,\"otherCallerCalls\":%llu,\"stackReadFailures\":%llu,"
                "\"protection\":%lu,\"samples\":[",
                trace.sampleCount ? "captured" : "no_samples", pid, creation, trace.elapsedMs,
                trace.totalCalls, trace.droppedCalls, trace.otherCallerCalls, trace.stackReadFailures,
                afterProtection);
    bool captureComplete = trace.sampleCount && !trace.droppedCalls && !trace.stackReadFailures;
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto& id = sample.identity;
        if (i) std::putchar(',');
        std::printf("{\"calls\":%llu,\"rootKind\":%lu,\"rowsAlias\":%lu,\"root\":",
                    sample.calls, static_cast<DWORD>(id.kind), static_cast<DWORD>(id.alias));
        const auto rootOkay = printTable(process, beforeModules, afterModules, id.root);
        std::fputs(",\"provider\":", stdout);
        const auto providerOkay = printTable(process, beforeModules, afterModules, id.provider);
        std::fputs(",\"rows\":", stdout);
        const auto rowsOkay = printTable(process, beforeModules, afterModules, id.rows);
        std::fputs(",\"site\":", stdout);
        const auto siteOkay = printTable(process, beforeModules, afterModules, id.site);
        captureComplete = captureComplete && id.kind != GridRootKind::Unknown &&
            rootOkay && providerOkay && rowsOkay && siteOkay;
        std::putchar('}');
    }
    std::printf("],\"captureComplete\":%s}\n", captureComplete ? "true" : "false");
    return 0;
}

int gridProbeCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    if (argc != 4 || std::wstring_view(argv[1]) != L"--grid-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return gridProbeToDesigner(pid, creation);
}
} // namespace

#ifndef GRID_PROBE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return gridProbeCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
