// Reuse the checked attach primitives. The production entry point is never dispatched
// by this executable: only the explicit PID + creation-time probe command is accepted.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "scroller_identity_probe.hpp"

namespace {
constexpr wchar_t scrollerProbeDllName[] = L"scroller-identity-probe.dll";

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

using ProbeProtections = std::array<DWORD, scrollerSites.size()>;

bool probeBytes(HANDLE process, ULONG_PTR wbase, ProbeProtections& protections)
{
    if (!wbase) return false;
    bool clean = true;
    for (size_t i = 0; i < scrollerSites.size(); ++i) {
        const auto& site = scrollerSites[i];
        if (wbase > (std::numeric_limits<ULONG_PTR>::max)() - site.rva) return false;
        const auto address = wbase + site.rva;
        MEMORY_BASIC_INFORMATION region{};
        std::array<BYTE, scrollerSites[0].signature.size()> bytes{};
        SIZE_T read{};
        if (!probeImageRegion(process, address, bytes.size(), wbase, true, region) ||
            region.Protect != PAGE_EXECUTE_READ ||
            !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(),
                               bytes.size(), &read) || read != bytes.size() ||
            bytes != site.signature) {
            clean = false;
            continue;
        }
        protections[i] = region.Protect;
    }
    return clean;
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

const ProbeModule* locationModule(HANDLE process, const std::vector<ProbeModule>& before,
                                 const std::vector<ProbeModule>& after, const ScrollerLocation& location,
                                 bool code)
{
    if (location.state != ScrollerLocationState::Image ||
        location.rva > (std::numeric_limits<ULONG_PTR>::max)() - location.imageBase) return nullptr;
    const SIZE_T length = code ? 1 : sizeof(ULONG_PTR);
    for (const auto& current : after) {
        if (current.base != location.imageBase || location.rva >= current.size ||
            length > current.size - location.rva) continue;
        for (const auto& original : before) {
            if (original.base != current.base || original.size != current.size ||
                !pathEqual(original.path, current.path.c_str())) continue;
            MEMORY_BASIC_INFORMATION region{};
            if (probeImageRegion(process, current.base + location.rva, length, current.base, code, region))
                return &current;
        }
    }
    return nullptr;
}

bool printLocation(HANDLE process, const std::vector<ProbeModule>& before,
                   const std::vector<ProbeModule>& after, const ScrollerLocation& location, bool code = false)
{
    const auto module = locationModule(process, before, after, location, code);
    std::printf("{\"state\":%lu,\"location\":", static_cast<DWORD>(location.state));
    printProbeAddress(module, location.imageBase + location.rva);
    std::putchar('}');
    return module != nullptr;
}

bool validLocation(const ScrollerLocation& location, bool code = false)
{
    if (location.state == ScrollerLocationState::Image)
        return location.imageBase && (code || !(location.rva % alignof(ULONG_PTR)));
    return location.state < ScrollerLocationState::Image && location.state != ScrollerLocationState::ReadFailed &&
        !location.imageBase && !location.rva;
}

bool validTrace(const ScrollerProbeTrace& trace, ULONG_PTR wbase)
{
    if (trace.size != sizeof(trace) || trace.version != scrollerProbeVersion ||
        trace.sampleCount > scrollerProbeCapacity) return false;
    std::array<ULONGLONG, scrollerSites.size()> accepted{}, unknown{};
    for (size_t i = 0; i < scrollerSites.size(); ++i) {
        accepted[i] = trace.counts[i].accepted;
        unknown[i] = trace.counts[i].unknownIdentities;
        if (unknown[i] > accepted[i]) return false;
    }
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto& id = sample.identity;
        const auto site = static_cast<size_t>(id.site);
        if (site >= scrollerSites.size() || !sample.calls || sample.calls > accepted[site] ||
            id.scroller.state != ScrollerLocationState::Image || id.scroller.imageBase != wbase ||
            !validLocation(id.scroller) || !id.dispatchMethodRva ||
            id.dispatchMethodRva != scrollerDrawRva(id.scroller.rva) ||
            id.window.state == ScrollerLocationState::NotRead || !validLocation(id.window) ||
            id.caller.state == ScrollerLocationState::NotRead || !validLocation(id.caller, true) ||
            !validLocation(id.component) || !validLocation(id.componentMethod, true)) return false;
        if (id.site == ScrollerSite::Dispatch) {
            if (id.component.state != ScrollerLocationState::NotRead ||
                id.componentMethod.state != ScrollerLocationState::NotRead ||
                id.componentOffset || id.drawFlag) return false;
        } else {
            if (id.component.state == ScrollerLocationState::NotRead ||
                id.componentMethod.state == ScrollerLocationState::NotRead ||
                (id.component.state != ScrollerLocationState::Image &&
                 id.componentMethod.state != ScrollerLocationState::Unmapped) ||
                id.componentOffset < 0x40 || id.componentOffset > 0x70 ||
                id.componentOffset % 0x10 || id.drawFlag > MAXBYTE) return false;
        }
        accepted[site] -= sample.calls;
        if (unresolvedScrollerIdentity(id)) {
            if (sample.calls > unknown[site]) return false;
            unknown[site] -= sample.calls;
        }
    }
    for (size_t i = 0; i < scrollerSites.size(); ++i)
        if (accepted[i] || unknown[i]) return false;
    return true;
}

struct ProbeCallResult {
    DWORD transportError{};
    DWORD workerResult{};
    bool received{};
    bool payloadReleased{};
};

using ProbeCall = DWORD (*)(HANDLE, ULONG_PTR, LPVOID, DWORD&);

ProbeCallResult callScrollerProbe(HANDLE process, ULONG_PTR run, ScrollerProbeTrace& trace,
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

int scrollerProbeReplyError(const ScrollerProbeTrace& trace, const ProbeCallResult& call,
                         const ProbeProtections& before, ULONG_PTR wbase)
{
    if (!call.received) return attachResult(AttachStatus::Ambiguous, call.transportError, call.workerResult);
    if (!validTrace(trace, wbase)) return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_DATA, call.workerResult);
    // A byte, cache or protection ambiguity takes priority over a payload leak.
    for (const auto& patch : trace.patches)
        if (patch.cleanupError) return attachResult(AttachStatus::Ambiguous, patch.cleanupError, call.workerResult);
    if (call.transportError || !call.payloadReleased)
        return attachResult(AttachStatus::Error, call.transportError, call.workerResult);
    if (call.workerResult || trace.patchError || trace.snapshotError)
        return attachResult(AttachStatus::Error, ERROR_SUCCESS, call.workerResult ? call.workerResult :
                            (trace.patchError ? trace.patchError : trace.snapshotError));
    for (size_t i = 0; i < before.size(); ++i)
        if (trace.patches[i].originalProtection != before[i] || trace.patches[i].finalProtection != before[i])
            return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    return 0;
}

int scrollerProbeToDesigner(DWORD pid, ULONGLONG creation)
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
    const auto dll = (directory / scrollerProbeDllName).wstring();
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules, scrollerProbeDllName))
        return attachError(process, creation, error);
    // Any previous load, even after an interrupted caller, prohibits automatic rearming.
    if (modules.competingDll || modules.ownBase)
        return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (modules.frame.empty() || modules.graphics.empty())
        return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    std::vector<ProbeModule> beforeModules;
    if (const auto error = probeModules(pid, beforeModules)) return attachError(process, creation, error);
    const ProbeModule* wbaseModule{};
    for (const auto& module : beforeModules) {
        if (!pathEqual(module.name, L"wbase83.dll")) continue;
        if (wbaseModule) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
        wbaseModule = &module;
    }
    if (!wbaseModule) return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    if (!matchesSha256(modules.frame, frameHash) || !matchesSha256(modules.graphics, graphicsHash) ||
        !matchesSha256(wbaseModule->path, scrollerWbaseHash))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (!productionThemePresent(process, beforeModules))
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);
    const auto wbase = wbaseModule->base;
    ProbeProtections beforeProtection{};
    if (!probeBytes(process, wbase, beforeProtection))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate(scrollerProbeExport, scrollerProbeAbiExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    ULONG_PTR unusedSetter{};
    const auto loadError = loadTheme(process, pid, dll, unusedSetter, scrollerProbeExport);
    if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
        return attachResult(AttachStatus::Ambiguous, loadError);
    if (loadError) return attachError(process, creation, loadError);
    if (const auto error = snapshotAttachModules(pid, dll, modules, scrollerProbeDllName))
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
    ProbeProtections justBeforeProtection{};
    if (!probeBytes(process, wbase, justBeforeProtection) || justBeforeProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);

    ScrollerProbeTrace trace{};
    const auto call = callScrollerProbe(process, run, trace);
    const auto result = call.workerResult;
    if (call.transportError == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, WAIT_TIMEOUT);
    ProbeProtections afterProtection{};
    if (checkProcessLifetime(process, creation) || !probeBytes(process, wbase, afterProtection) ||
        afterProtection != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, result);
    if (const auto error = scrollerProbeReplyError(trace, call, beforeProtection, wbase)) return error;
    std::vector<ProbeModule> afterModules;
    if (const auto error = probeModules(pid, afterModules))
        return attachResult(AttachStatus::Error, error);
    std::printf("{\"status\":\"%s\",\"cleanupVerified\":true,\"payloadReleased\":true,\"pid\":%lu,"
                "\"creation\":\"%llu\",\"elapsedMs\":%lu,\"printingClassification\":\"not-established\","
                "\"componentHookScope\":\"wbase83+0x1e860-only\",\"sites\":[",
                trace.sampleCount ? "captured" : "no_samples", pid, creation, trace.elapsedMs);
    bool captureComplete = true;
    for (size_t i = 0; i < trace.counts.size(); ++i) {
        const auto& counts = trace.counts[i];
        if (i) std::putchar(',');
        std::printf("{\"site\":%zu,\"rva\":\"0x%lx\",\"protection\":%lu,\"accepted\":%llu,"
                    "\"mismatches\":%llu,\"readFailures\":%llu,\"unknownIdentities\":%llu,"
                    "\"capacityDrops\":%llu,\"busyDrops\":%llu}",
                    i, scrollerSites[i].rva, afterProtection[i], counts.accepted, counts.mismatches,
                    counts.readFailures, counts.unknownIdentities, counts.capacityDrops, counts.busyDrops);
        captureComplete = captureComplete && counts.accepted && !counts.mismatches && !counts.readFailures &&
            !counts.unknownIdentities && !counts.capacityDrops && !counts.busyDrops;
    }
    std::fputs("],\"samples\":[", stdout);
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        const auto& id = sample.identity;
        if (i) std::putchar(',');
        std::printf("{\"site\":%lu,\"calls\":%llu,\"rect\":[%ld,%ld,%ld,%ld],"
                    "\"dispatchMethodRva\":\"0x%lx\",\"scroller\":",
                    static_cast<DWORD>(id.site), sample.calls, id.rect.left, id.rect.top,
                    id.rect.right, id.rect.bottom, id.dispatchMethodRva);
        const auto scrollerOkay = printLocation(process, beforeModules, afterModules, id.scroller);
        std::fputs(",\"window\":", stdout);
        const auto windowOkay = printLocation(process, beforeModules, afterModules, id.window);
        std::fputs(",\"caller\":", stdout);
        const auto callerOkay = printLocation(process, beforeModules, afterModules, id.caller, true);
        captureComplete = captureComplete && scrollerOkay && windowOkay && callerOkay;
        if (id.site == ScrollerSite::Component) {
            std::printf(",\"componentOffset\":%lu,\"drawFlag\":%lu,\"component\":",
                        id.componentOffset, id.drawFlag);
            const auto componentOkay = printLocation(process, beforeModules, afterModules, id.component);
            std::fputs(",\"componentMethod\":", stdout);
            const auto methodOkay = printLocation(process, beforeModules, afterModules, id.componentMethod, true);
            captureComplete = captureComplete && componentOkay && methodOkay;
        }
        std::putchar('}');
    }
    std::printf("],\"captureComplete\":%s}\n", captureComplete ? "true" : "false");
    return 0;
}

int scrollerProbeCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    if (argc != 4 || std::wstring_view(argv[1]) != L"--scroller-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return scrollerProbeToDesigner(pid, creation);
}
} // namespace

#ifndef SCROLLER_PROBE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return scrollerProbeCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
