// Reuse the checked attach primitives. The production entry point is never dispatched
// by this executable: only the explicit PID + creation-time probe command is accepted.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "search_counter_probe.hpp"

namespace {
constexpr wchar_t counterProbeDllName[] = L"search-counter-probe.dll";

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

const ProbeModule* containingModule(const std::vector<ProbeModule>& modules,
                                   ULONG_PTR address, SIZE_T length = 1)
{
    for (const auto& module : modules)
        if (address >= module.base && address - module.base < module.size &&
            length <= module.size - (address - module.base)) return &module;
    return nullptr;
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

bool probeBytes(HANDLE process, ULONG_PTR frontend, DWORD& protection)
{
    MEMORY_BASIC_INFORMATION region{};
    const auto address = frontend + counterProbeRva;
    std::array<BYTE, counterProbeSignature.size()> bytes{};
    SIZE_T read{};
    if (!probeImageRegion(process, address, bytes.size(), frontend, true, region) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(),
                           bytes.size(), &read) || read != bytes.size() ||
        bytes != counterProbeSignature) return false;
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

struct ProbeResolution {
    const ProbeModule* tableModule{};
    const ProbeModule* setterModule{};
    ULONG_PTR setter{};
};

ProbeResolution resolveProbeSample(HANDLE process, const std::vector<ProbeModule>& modules,
                                   const CounterProbeSample& sample)
{
    ProbeResolution result;
    if (sample.table > (std::numeric_limits<ULONG_PTR>::max)() - 0x48 ||
        sample.table % alignof(ULONG_PTR)) return result;
    const auto slot = sample.table + 0x40;
    const auto table = containingModule(modules, sample.table, 0x48);
    MEMORY_BASIC_INFORMATION region{};
    if (!table || !probeImageRegion(process, sample.table, 0x48, table->base, false, region))
        return result;
    SIZE_T read{};
    ULONG_PTR setter{};
    // Exactly one image slot, after cleanup. Never read the receiver or its text.
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(slot), &setter,
                           sizeof(setter), &read) || read != sizeof(setter)) return result;
    result.tableModule = table;
    const auto code = containingModule(modules, setter);
    if (!code || !probeImageRegion(process, setter, 1, code->base, true, region)) return result;
    result.setterModule = code;
    result.setter = setter;
    return result;
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

struct ProbeCallResult {
    DWORD transportError{};
    DWORD workerResult{};
    bool received{};
    bool payloadReleased{};
};

using ProbeCall = DWORD (*)(HANDLE, ULONG_PTR, LPVOID, DWORD&);

ProbeCallResult callCounterProbe(HANDLE process, ULONG_PTR run, CounterProbeTrace& trace,
                                 ProbeCall call = remoteCall)
{
    ProbeCallResult outcome;
    const auto payload = VirtualAllocEx(process, nullptr, sizeof(trace), MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
    if (!payload) { outcome.transportError = GetLastError(); return outcome; }
    outcome.transportError = WriteProcessMemory(process, payload, &trace, sizeof(trace), nullptr) ?
        call(process, run, payload, outcome.workerResult) : GetLastError();
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

int counterProbeReplyError(const CounterProbeTrace& trace, const ProbeCallResult& call,
                           DWORD beforeProtection)
{
    if (!call.received) return attachResult(AttachStatus::Ambiguous, call.transportError, call.workerResult);
    // An uncertain instruction-cache/patch cleanup takes priority over a payload leak.
    if (trace.size != sizeof(trace) || trace.version != counterProbeVersion ||
        trace.sampleCount > counterProbeCapacity || trace.cleanupError)
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

int counterProbeToDesigner(DWORD pid, ULONGLONG creation)
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
    const auto dll = (directory / counterProbeDllName).wstring();
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules, counterProbeDllName))
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
    const auto frontend = remoteModule(pid, modules.frontend);
    DWORD beforeProtection{};
    if (!frontend || !probeBytes(process, frontend, beforeProtection))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate(counterProbeExport, counterProbeAbiExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    ULONG_PTR unusedSetter{};
    const auto loadError = loadTheme(process, pid, dll, unusedSetter, counterProbeExport);
    if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
        return attachResult(AttachStatus::Ambiguous, loadError);
    if (loadError) return attachError(process, creation, loadError);
    if (const auto error = snapshotAttachModules(pid, dll, modules, counterProbeDllName))
        return attachResult(AttachStatus::Ambiguous, error);
    if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    const auto run = image.remoteSetter(process, modules);
    if (!run) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    CounterProbeTrace trace{};
    trace.size = sizeof(trace);
    trace.version = counterProbeVersion;
    const auto call = callCounterProbe(process, run, trace);
    const auto result = call.workerResult;
    if (call.transportError == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, WAIT_TIMEOUT);
    DWORD afterProtection{};
    if (checkProcessLifetime(process, creation) || !probeBytes(process, frontend, afterProtection) ||
        afterProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, result);
    if (const auto error = counterProbeReplyError(trace, call, beforeProtection)) return error;
    std::vector<ProbeModule> afterModules;
    if (const auto error = probeModules(pid, afterModules))
        return attachResult(AttachStatus::Error, error);
    std::printf("{\"status\":\"%s\",\"cleanupVerified\":true,\"payloadReleased\":true,\"pid\":%lu,"
                "\"creation\":\"%llu\",\"elapsedMs\":%lu,\"totalCalls\":%lu,"
                "\"droppedCalls\":%lu,\"protection\":%lu,\"samples\":[",
                trace.sampleCount ? "captured" : "no_samples", pid, creation, trace.elapsedMs,
                trace.totalCalls, trace.droppedCalls, afterProtection);
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto resolved = resolveProbeSample(process, afterModules, sample);
        if (i) std::putchar(',');
        std::printf("{\"receiver\":\"0x%llx\",\"tableAddress\":\"0x%llx\","
                    "\"thread\":%lu,\"calls\":%lu,\"table\":",
                    sample.receiver, sample.table, sample.thread, sample.calls);
        printProbeAddress(resolved.tableModule, sample.table);
        std::fputs(",\"setter\":", stdout);
        printProbeAddress(resolved.setterModule, resolved.setter);
        std::putchar('}');
    }
    std::fputs("]}\n", stdout);
    return 0;
}

int counterProbeCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    if (argc != 4 || std::wstring_view(argv[1]) != L"--counter-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return counterProbeToDesigner(pid, creation);
}
} // namespace

#ifndef COUNTER_PROBE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return counterProbeCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
