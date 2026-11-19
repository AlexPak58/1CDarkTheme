// Reuse the checked attach primitives. The production entry point is never dispatched
// by this executable: only the explicit PID + creation-time probe command is accepted.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "png_surface_probe.hpp"
#include <cmath>

namespace {
constexpr wchar_t pngSurfaceDllName[] = L"scroller-png-surface-probe.dll";
constexpr auto admittedThemeHash = L"F86587C81C5CC33C6D240A0B29E1A636C1AC8279A83702D354FC4FB390A9392D";
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

struct ProbeCallResult {
    DWORD transportError{};
    DWORD workerResult{};
    bool received{};
    bool payloadReleased{};
};

using ProbeCall = DWORD (*)(HANDLE, ULONG_PTR, LPVOID, DWORD&);

DWORD remotePngSurfaceCall(HANDLE process, ULONG_PTR address, LPVOID payload, DWORD& result)
{
    const Handle worker(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), payload, 0, nullptr));
    if (!static_cast<HANDLE>(worker)) return GetLastError();
    if (WaitForSingleObject(worker, pngSurfaceCaptureMs + 15000) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
    return GetExitCodeThread(worker, &result) ? ERROR_SUCCESS : WAIT_TIMEOUT;
}

ProbeCallResult callPngSurfaceProbe(HANDLE process, ULONG_PTR run, PngSurfaceTrace& trace,
                                 ProbeCall call = remotePngSurfaceCall)
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


const ProbeModule* uniqueModule(const std::vector<ProbeModule>& modules, const wchar_t* name)
{
    const ProbeModule* found{};
    for (const auto& module : modules) {
        if (!pathEqual(module.name, name)) continue;
        if (found) return nullptr;
        found = &module;
    }
    return found;
}

bool admittedThemePresent(HANDLE process, const std::vector<ProbeModule>& modules)
{
    const auto theme = uniqueModule(modules, L"tree-theme.dll");
    if (!theme) return false;
    const Handle file(CreateFileW(theme->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(file) == INVALID_HANDLE_VALUE ||
        !matchesSha256(theme->path, admittedThemeHash)) return false;
    AttachImage image(theme->path);
    AttachModules mapped;
    mapped.ownBase = theme->base;
    mapped.ownSize = theme->size;
    // This historical probe's fixed production hash belongs specifically to v9.
    return image.validate("SetThemeProfile", "ThemeProfileAttachV9") && image.remoteSetter(process, mapped);
}

bool originalPngIat(HANDLE process, const ProbeModule& graphics, const ProbeModule& cairo,
                    DWORD& protection)
{
    if (pngSurfaceIatRva + sizeof(ULONG_PTR) > graphics.size ||
        pngSurfaceOriginalRva >= cairo.size ||
        graphics.base > MAXUINT_PTR - pngSurfaceIatRva ||
        cairo.base > MAXUINT_PTR - pngSurfaceOriginalRva) return false;
    MEMORY_BASIC_INFORMATION region{}, codeRegion{};
    ULONG_PTR pointer{};
    SIZE_T read{};
    const auto slot = graphics.base + pngSurfaceIatRva;
    if (slot % alignof(ULONG_PTR) ||
        !probeImageRegion(process, slot, sizeof(pointer), graphics.base, false, region) ||
        !probeImageRegion(process, cairo.base + pngSurfaceOriginalRva, 1, cairo.base, true, codeRegion) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(slot), &pointer, sizeof(pointer), &read) ||
        read != sizeof(pointer) || pointer != cairo.base + pngSurfaceOriginalRva) return false;
    protection = region.Protect;
    return true;
}

bool validSurfaceIdentity(const PngSurfaceIdentity& id)
{
    const bool tree = id.windowTableRva == 0xA885D0 && id.scrollerTableRva == 0x3CD00;
    const bool editor = id.windowTableRva == 0xABEDB0 && id.scrollerTableRva == 0x3CA78;
    if ((!tree && !editor) || id.reserved || id.orientation > 1 || id.hotId > 5 || id.pressedId > 5 ||
        (id.part != 0 && id.part != 1 && id.part != 3) ||
        id.componentOffset != 0x40 + id.orientation * 0x10 || (tree && id.orientation) ||
        id.right <= id.left || id.bottom <= id.top || id.status || id.surfaceType ||
        id.width <= 0 || id.width > 32767 || id.height <= 0 || id.height > 32767 ||
        id.format < 0 || id.format > 5 || id.stride <= 0 || id.stride > 1024 * 1024 || id.stride % 4 ||
        !std::isfinite(id.deviceScaleX) || id.deviceScaleX <= 0 ||
        !std::isfinite(id.deviceScaleY) || id.deviceScaleY <= 0 ||
        !std::isfinite(id.deviceOffsetX) || !std::isfinite(id.deviceOffsetY)) return false;
    constexpr std::array<LONG, 6> bits{32, 32, 8, 1, 16, 32};
    const auto minimum = ((id.width * bits[id.format] + 31) / 32) * 4;
    return id.stride >= minimum;
}

bool validPngTrace(const PngSurfaceTrace& trace)
{
    if (trace.size != sizeof(trace) || trace.version != pngSurfaceProbeVersion || trace.reserved ||
        trace.sampleCount > pngSurfaceCapacity ||
        (trace.capacityDrops && trace.sampleCount != pngSurfaceCapacity)) return false;
    auto remaining = trace.stages[static_cast<size_t>(PngSurfaceStage::Accepted)];
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& sample = trace.samples[i];
        if (!sample.calls || sample.calls > remaining || !validSurfaceIdentity(sample.identity)) return false;
        for (DWORD j = 0; j < i; ++j)
            if (sample.identity == trace.samples[j].identity) return false;
        remaining -= sample.calls;
    }
    // Accepted counts include samples discarded only after the fixed buffer fills.
    return remaining == trace.capacityDrops;
}

int pngSurfaceReplyError(const PngSurfaceTrace& trace, const ProbeCallResult& call, DWORD before)
{
    if (!call.received || !validPngTrace(trace))
        return attachResult(AttachStatus::Ambiguous, call.transportError ? call.transportError : ERROR_INVALID_DATA,
                            call.workerResult);
    if (trace.cleanupError) return attachResult(AttachStatus::Ambiguous, trace.cleanupError, call.workerResult);
    if (call.transportError || !call.payloadReleased)
        return attachResult(AttachStatus::Error, call.transportError ? call.transportError : ERROR_INVALID_STATE,
                            call.workerResult);
    if (call.workerResult || trace.patchError || trace.snapshotError)
        return attachResult(AttachStatus::Error, ERROR_SUCCESS, call.workerResult ? call.workerResult :
                            (trace.patchError ? trace.patchError : trace.snapshotError));
    if (trace.originalProtection != before || trace.finalProtection != before ||
        trace.elapsedMs < pngSurfaceCaptureMs)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    return 0;
}

void printPngTrace(const PngSurfaceTrace& trace, DWORD pid, ULONGLONG creation)
{
    std::printf("{\"status\":\"%s\",\"probeVersion\":%lu,\"pid\":%lu,\"creation\":\"%llu\","
                "\"elapsedMs\":%lu,\"cleanupVerified\":true,\"payloadReleased\":true,"
                "\"scope\":\"tree-editor-track-arrows\",\"thumbIncluded\":false,"
                "\"originalProtection\":%lu,\"finalProtection\":%lu,\"busyDrops\":%llu,\"capacityDrops\":%llu,\"stages\":{",
                trace.sampleCount ? "captured" : "no_samples", trace.version, pid, creation, trace.elapsedMs,
                trace.originalProtection, trace.finalProtection, trace.busyDrops, trace.capacityDrops);
    constexpr std::array names{"otherCall", "graphicsChain", "paintRoute", "owner", "component", "readFailure",
                               "surfaceError", "surfaceType", "invalidMetrics", "accepted"};
    static_assert(names.size() == static_cast<size_t>(PngSurfaceStage::Count));
    for (size_t i = 0; i < names.size(); ++i)
        std::printf("%s\"%s\":%llu", i ? "," : "", names[i], trace.stages[i]);
    std::fputs("},\"samples\":[", stdout);
    unsigned coverage{};
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& s = trace.samples[i];
        const auto& id = s.identity;
        const auto owner = id.scrollerTableRva == 0x3CD00 ? 0 : id.orientation ? 2 : 1;
        const auto part = id.part == 3 ? 2 : id.part;
        coverage |= 1u << (owner * 3 + part);
        std::printf("%s{\"calls\":%llu,\"windowTableRva\":%lu,\"scrollerTableRva\":%lu,"
                    "\"componentOffset\":%lu,\"part\":%lu,\"orientation\":%lu,\"hotId\":%lu,\"pressedId\":%lu,"
                    "\"rect\":[%ld,%ld,%ld,%ld],\"surfaceType\":%ld,\"format\":%ld,\"width\":%ld,\"height\":%ld,"
                    "\"stride\":%ld,\"status\":%ld,\"deviceScale\":[%.17g,%.17g],\"deviceOffset\":[%.17g,%.17g]}",
                    i ? "," : "", s.calls, id.windowTableRva, id.scrollerTableRva, id.componentOffset,
                    id.part, id.orientation, id.hotId, id.pressedId, id.left, id.top, id.right, id.bottom,
                    id.surfaceType, id.format, id.width, id.height, id.stride, id.status,
                    id.deviceScaleX, id.deviceScaleY, id.deviceOffsetX, id.deviceOffsetY);
    }
    const bool clean = !trace.busyDrops && !trace.capacityDrops &&
        !trace.stages[static_cast<size_t>(PngSurfaceStage::ReadFailure)] &&
        !trace.stages[static_cast<size_t>(PngSurfaceStage::SurfaceError)] &&
        !trace.stages[static_cast<size_t>(PngSurfaceStage::SurfaceType)] &&
        !trace.stages[static_cast<size_t>(PngSurfaceStage::InvalidMetrics)];
    std::printf("],\"partCoverageMask\":%u,\"captureComplete\":%s}\n", coverage,
                coverage == 0x1FF && clean ? "true" : "false");
}
int pngSurfaceToDesigner(DWORD pid, ULONGLONG creation)
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
    const auto dll = (directory / pngSurfaceDllName).wstring();
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules, pngSurfaceDllName))
        return attachError(process, creation, error);
    if (modules.competingDll || modules.ownBase)
        return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (const auto error = validateAttachModules(modules, matchesSha256))
        return attachResult(AttachStatus::Unsupported, error);
    std::vector<ProbeModule> beforeModules;
    if (const auto error = probeModules(pid, beforeModules)) return attachError(process, creation, error);
    const auto graphics = uniqueModule(beforeModules, L"grphcs.dll");
    const auto cairo = uniqueModule(beforeModules, L"cairo.dll");
    if (!graphics || !cairo) return attachError(process, creation, ERROR_MOD_NOT_FOUND);
    if (!matchesSha256(cairo->path, pngSurfaceCairoHash))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (!admittedThemePresent(process, beforeModules))
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);
    DWORD beforeProtection{};
    if (!originalPngIat(process, *graphics, *cairo, beforeProtection))
        return attachResult(AttachStatus::Conflict, ERROR_INVALID_STATE);
    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate(pngSurfaceRunExport, pngSurfaceAbiExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    ULONG_PTR unusedSetter{};
    const auto loadError = loadTheme(process, pid, dll, unusedSetter, pngSurfaceRunExport);
    if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
        return attachResult(AttachStatus::Ambiguous, loadError);
    if (loadError) return attachError(process, creation, loadError);
    if (const auto error = snapshotAttachModules(pid, dll, modules, pngSurfaceDllName))
        return attachResult(AttachStatus::Ambiguous, error);
    if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    const auto run = image.remoteSetter(process, modules);
    if (!run) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    DWORD justBefore{};
    if (checkProcessLifetime(process, creation) ||
        !originalPngIat(process, *graphics, *cairo, justBefore) || justBefore != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    PngSurfaceTrace trace{};
    const auto call = callPngSurfaceProbe(process, run, trace);
    if (call.transportError == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, WAIT_TIMEOUT);
    DWORD after{};
    if (checkProcessLifetime(process, creation) ||
        !originalPngIat(process, *graphics, *cairo, after) || after != beforeProtection)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, call.workerResult);
    if (const auto error = pngSurfaceReplyError(trace, call, beforeProtection)) return error;
    printPngTrace(trace, pid, creation);
    return 0;
}

int pngSurfaceCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    if (argc != 4 || std::wstring_view(argv[1]) != L"--png-surface-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return pngSurfaceToDesigner(pid, creation);
}
} // namespace

#ifndef PNG_SURFACE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return pngSurfaceCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
