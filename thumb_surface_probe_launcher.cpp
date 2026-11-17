// The production command dispatcher is not an entry point in this executable.
#define wmain unusedProductionLauncherEntry
#include "launcher.cpp"
#undef wmain
#include "thumb_surface_probe.hpp"
#include "png_surface_probe.hpp"
#include <cmath>

namespace {
constexpr wchar_t thumbThemeName[] = L"tree-theme.dll";

enum ThumbModuleIndex : size_t { ThumbTheme, ThumbFrame, ThumbGraphics, ThumbFrontend,
                                  ThumbWbase, ThumbCairo, ThumbImageUi, ThumbImage, ThumbModulesCount };
constexpr std::array thumbModuleNames{thumbThemeName, L"frame.dll", L"grphcs.dll", L"frntend.dll",
                                     L"wbase83.dll", L"cairo.dll", L"imageui.dll", L"image.dll"};
static_assert(thumbModuleNames.size() == ThumbModulesCount);

struct ThumbModule {
    ULONG_PTR base{};
    DWORD size{};
    std::wstring path;
};
using ThumbModules = std::array<ThumbModule, ThumbModulesCount>;

DWORD captureThumbModules(DWORD pid, ThumbModules& modules)
{
    modules = {};
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    if (static_cast<HANDLE>(snapshot) == INVALID_HANDLE_VALUE) return GetLastError();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry)) return GetLastError();
    do {
        for (size_t i = 0; i < thumbModuleNames.size(); ++i) {
            if (!pathEqual(thumbModuleNames[i], entry.szModule)) continue;
            auto& module = modules[i];
            if (module.base) return ERROR_ALREADY_EXISTS;
            module = {reinterpret_cast<ULONG_PTR>(entry.modBaseAddr), entry.modBaseSize, entry.szExePath};
            break;
        }
    } while (Module32NextW(snapshot, &entry));
    const auto error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}

bool thumbModulePath(const ThumbModule& module, const std::wstring& dll)
{
    return module.base && module.size && pathEqual(module.path, dll.c_str());
}

bool sameThumbModules(const ThumbModules& before, const ThumbModules& after)
{
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i].base != after[i].base || before[i].size != after[i].size ||
            !pathEqual(before[i].path, after[i].path.c_str())) return false;
    }
    return true;
}

template<class HashCheck>
DWORD validateThumbPlatformModules(const ThumbModules& modules, HashCheck matches)
{
    constexpr std::array hashes{frameHash, graphicsHash, listFrontendHash, wbaseHash, cairoHash,
                                thumbSurfaceImageUiHash, thumbSurfaceImageHash};
    static_assert(hashes.size() + 1 == ThumbModulesCount);
    for (size_t i = ThumbFrame; i < modules.size(); ++i) {
        const auto& module = modules[i];
        if (!module.base || !module.size || module.path.empty()) return ERROR_MOD_NOT_FOUND;
        if (!matches(module.path, hashes[i - ThumbFrame])) return ERROR_REVISION_MISMATCH;
    }
    return ERROR_SUCCESS;
}

bool thumbExpectedHash(std::wstring_view argument, Sha256Hex& digest)
{
    if (argument.size() != digest.size() - 1) return false;
    for (size_t i = 0; i < argument.size(); ++i) {
        const auto ch = argument[i];
        if (ch >= L'a' && ch <= L'f') digest[i] = static_cast<wchar_t>(ch - L'a' + L'A');
        else if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F')) digest[i] = ch;
        else return false;
    }
    digest.back() = L'\0';
    return true;
}

bool thumbImageRegion(HANDLE process, const ThumbModule& image, ULONG_PTR address,
                      SIZE_T length, DWORD protection)
{
    if (!image.base || !length || address < image.base || address - image.base >= image.size ||
        length > image.size - (address - image.base)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.Protect == protection &&
        reinterpret_cast<ULONG_PTR>(region.AllocationBase) == image.base && address >= start &&
        address - start < region.RegionSize && length <= region.RegionSize - (address - start);
}

ULONG_PTR thumbHookPointer(HANDLE process, const ThumbModule& graphics, const ThumbModule& theme)
{
    if (graphics.base > MAXUINT_PTR - pngSurfaceIatRva) return 0;
    const auto slot = graphics.base + pngSurfaceIatRva;
    ULONG_PTR pointer{};
    SIZE_T read{};
    if (slot % alignof(ULONG_PTR) || !thumbImageRegion(process, graphics, slot, sizeof(pointer), PAGE_READONLY) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(slot), &pointer, sizeof(pointer), &read) ||
        read != sizeof(pointer) || !thumbImageRegion(process, theme, pointer, 1, PAGE_EXECUTE_READ)) return 0;
    return pointer;
}

bool thumbHookUnchanged(HANDLE process, const ThumbModules& after, ULONG_PTR before)
{
    return before && thumbHookPointer(process, after[ThumbGraphics], after[ThumbTheme]) == before;
}

ULONG_PTR thumbMappedRun(HANDLE process, AttachImage& image, const ThumbModule& theme)
{
    AttachModules mapped;
    mapped.ownBase = theme.base;
    mapped.ownSize = theme.size;
    const auto checked = [&](const char* name, const char* abi) -> ULONG_PTR {
        if (!image.validate(name, abi)) return 0;
        const auto rvaAddress = image.remoteSetter(process, mapped);
        return thumbImageRegion(process, theme, rvaAddress, 1, PAGE_EXECUTE_READ) ? rvaAddress : 0;
    };
    if (!checked("SetThemeProfile", themeProfileAttachExport) ||
        !checked(themeProfileAttachExport, "SetThemeProfile") ||
        !checked(thumbSurfaceAbiExport, thumbSurfaceRunExport)) return 0;
    return checked(thumbSurfaceRunExport, thumbSurfaceAbiExport);
}

struct ThumbWorkerResult {
    DWORD error{}, exitCode{};
    bool mayUsePayload{};
};
using ThumbWorkerCall = ThumbWorkerResult (*)(HANDLE, ULONG_PTR, LPVOID);

ThumbWorkerResult remoteThumbSurfaceCall(HANDLE process, ULONG_PTR address, LPVOID payload)
{
    const Handle worker(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), payload, 0, nullptr));
    if (!static_cast<HANDLE>(worker)) return {GetLastError()};
    const auto wait = WaitForSingleObject(worker, thumbSurfaceCaptureMs + 15000);
    if (wait != WAIT_OBJECT_0) return {wait == WAIT_FAILED ? GetLastError() : wait, 0, true};
    DWORD exitCode{};
    if (!GetExitCodeThread(worker, &exitCode)) return {GetLastError()};
    return {ERROR_SUCCESS, exitCode};
}

struct ThumbCallResult {
    ThumbWorkerResult worker;
    DWORD transportError{};
    bool received{}, payloadReleased{};
};

ThumbCallResult callThumbSurfaceProbe(HANDLE process, ULONG_PTR run, ThumbSurfaceTrace& trace,
                                      ThumbWorkerCall call = remoteThumbSurfaceCall)
{
    ThumbCallResult outcome;
    const auto payload = VirtualAllocEx(process, nullptr, sizeof(trace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!payload) { outcome.transportError = GetLastError(); return outcome; }
    SIZE_T written{};
    if (!WriteProcessMemory(process, payload, &trace, sizeof(trace), &written))
        outcome.transportError = GetLastError();
    else if (written != sizeof(trace)) outcome.transportError = ERROR_PARTIAL_COPY;
    else {
        outcome.worker = call(process, run, payload);
        outcome.transportError = outcome.worker.error;
    }
    // Once started, a worker without a completed wait can still own the payload.
    if (outcome.worker.mayUsePayload) return outcome;
    if (!outcome.transportError) {
        SIZE_T read{};
        if (!ReadProcessMemory(process, payload, &trace, sizeof(trace), &read))
            outcome.transportError = GetLastError();
        else if (read != sizeof(trace)) outcome.transportError = ERROR_PARTIAL_COPY;
        else outcome.received = true;
    }
    outcome.payloadReleased = VirtualFreeEx(process, payload, 0, MEM_RELEASE) != FALSE;
    if (!outcome.payloadReleased && !outcome.transportError) outcome.transportError = GetLastError();
    return outcome;
}

constexpr std::array<DWORD, 7> thumbDrawReturns{0x19AF5, 0x19B78, 0x19BFD, 0x19ED0, 0x19FAD, 0x1A030, 0x1A0B5};
constexpr unsigned thumbVerticalPieces = 0x07, thumbHorizontalPieces = 0x70, thumbSharedPiece = 0x08;

unsigned thumbPiece(DWORD drawReturn)
{
    const auto match = std::find(thumbDrawReturns.begin(), thumbDrawReturns.end(), drawReturn);
    return match == thumbDrawReturns.end() ? 0 : 1u << (match - thumbDrawReturns.begin());
}

bool validThumbSurfaceIdentity(const ThumbSurfaceIdentity& id)
{
    const bool tree = id.windowTableRva == 0xA885D0 && id.scrollerTableRva == 0x3CD00;
    const bool editor = id.windowTableRva == 0xABEDB0 && id.scrollerTableRva == 0x3CA78;
    const DWORD cacheState = !id.enabled ? 1 : !id.interactive ? 0 : id.pressedId == 5 ? 3 : id.hotId == 5 ? 2 : 0;
    const auto allowedPieces = (id.orientation ? thumbHorizontalPieces : thumbVerticalPieces) | thumbSharedPiece;
    if ((!tree && !editor) || id.reserved1 || id.reserved2 || id.orientation > 1 || (tree && id.orientation) ||
        id.componentOffset != 0x40 + id.orientation * 0x10 || id.hotId > 5 || id.pressedId > 5 ||
        id.cacheState != cacheState || id.enabled > 1 || id.interactive > 1 ||
        !(thumbPiece(id.drawReturnRva) & allowedPieces) ||
        id.right <= id.left || id.bottom <= id.top || id.thumbRight <= id.thumbLeft ||
        id.thumbBottom <= id.thumbTop || id.status || id.surfaceType ||
        id.width <= 0 || id.width > 32767 || id.height <= 0 || id.height > 32767 ||
        id.format < 0 || id.format > 5 || id.stride <= 0 || id.stride > 1024 * 1024 || id.stride % 4 ||
        !std::isfinite(id.deviceScaleX) || id.deviceScaleX <= 0 ||
        !std::isfinite(id.deviceScaleY) || id.deviceScaleY <= 0 ||
        !std::isfinite(id.deviceOffsetX) || !std::isfinite(id.deviceOffsetY)) return false;
    constexpr std::array<LONG, 6> bits{32, 32, 8, 1, 16, 32};
    const auto minStride = ((id.width * bits[id.format] + 31) / 32) * 4;
    return id.stride >= minStride;
}

ULONGLONG thumbStage(const ThumbSurfaceTrace& trace, ThumbSurfaceStage stage)
{
    return trace.stages[static_cast<size_t>(stage)];
}

constexpr std::array thumbStageNames{
    "otherCall", "graphicsChain", "drawReturn", "delegatedDraw", "delegatedFrames", "paintReturn", "ownerReturn",
    "outerReturn", "vdc", "orientation", "thumbPresent", "owner", "component", "cache", "state", "readFailure",
    "surfaceError", "surfaceType", "invalidMetrics", "accepted"
};
static_assert(thumbStageNames.size() == static_cast<size_t>(ThumbSurfaceStage::Count));

constexpr std::array thumbCacheNames{
    "inputsReadFailure", "nullCache", "invalidDrawQi", "drawingObjectMismatch", "nullRasterQi", "tablesReadFailure",
    "originalPair", "alternateDrawPair", "alternateRasterPair", "alternateBothPair", "unknownPair",
    "methodsReadFailure", "wrongDrawMethod", "wrongRasterMethod", "wrongBothMethods", "methodsMatch"
};
static_assert(thumbCacheNames.size() == static_cast<size_t>(ThumbCacheCounter::Count));

bool validThumbCacheCounters(const ThumbSurfaceTrace& trace)
{
    using Counter = ThumbCacheCounter;
    using Stage = ThumbSurfaceStage;
    const auto count = [&](Counter counter) { return trace.cacheCounters[static_cast<size_t>(counter)]; };
    const auto subtract = [](ULONGLONG& remaining, ULONGLONG value) {
        if (value > remaining) return false;
        remaining -= value;
        return true;
    };

    // Every early predicate rejection, unknown pair and alternate pair is Cache.
    // Any remaining Cache calls must be original-pair method mismatches.
    auto originalWrong = thumbStage(trace, Stage::Cache);
    for (const auto counter : {Counter::NullCache, Counter::InvalidDrawQi, Counter::DrawingObjectMismatch,
                              Counter::NullRasterQi, Counter::UnknownPair}) {
        if (!subtract(originalWrong, count(counter))) return false;
    }
    const auto beforeAlternates = originalWrong;
    for (const auto counter : {Counter::AlternateDrawPair, Counter::AlternateRasterPair, Counter::AlternateBothPair}) {
        if (!subtract(originalWrong, count(counter))) return false;
    }
    const auto alternates = beforeAlternates - originalWrong;
    auto originalRemaining = count(Counter::OriginalPair);
    if (!subtract(originalRemaining, originalWrong) ||
        count(Counter::OriginalPair) > (std::numeric_limits<ULONGLONG>::max)() - alternates)
        return false;

    // Each known pair records exactly one method outcome, without multiplying
    // or summing the untrusted counters before checking their bounds.
    auto remaining = count(Counter::OriginalPair) + alternates;
    const auto knownPairs = remaining;
    for (const auto counter : {Counter::WrongDrawMethod, Counter::WrongRasterMethod, Counter::WrongBothMethods}) {
        if (!subtract(remaining, count(counter))) return false;
    }
    auto alternateWrong = knownPairs - remaining;
    if (!subtract(alternateWrong, originalWrong) || alternateWrong > alternates ||
        !subtract(remaining, count(Counter::MethodsReadFailure)) || remaining != count(Counter::MethodsMatch)) return false;

    const auto alternateReadLimit = alternates - alternateWrong;
    const auto methodsRead = count(Counter::MethodsReadFailure);
    const auto originalReadMin = methodsRead > alternateReadLimit ? methodsRead - alternateReadLimit : 0;
    auto readFailures = thumbStage(trace, Stage::ReadFailure);
    if (!subtract(readFailures, count(Counter::InputsReadFailure)) ||
        !subtract(readFailures, count(Counter::TablesReadFailure)) || !subtract(readFailures, originalReadMin) ||
        !subtract(originalRemaining, originalReadMin)) return false;

    // These stages can only occur after an admitted original pair.
    for (const auto stage : {Stage::State, Stage::ThumbPresent, Stage::SurfaceError, Stage::SurfaceType,
                             Stage::InvalidMetrics, Stage::Accepted}) {
        if (!subtract(originalRemaining, thumbStage(trace, stage))) return false;
    }
    // Geometry and guarded reads also reject after the cache gate, but share
    // stage counters with earlier checks. Those counters bound the remainder.
    return originalRemaining <= readFailures || originalRemaining - readFailures <= thumbStage(trace, Stage::Owner);
}

bool validThumbTrace(const ThumbSurfaceTrace& trace)
{
    if (trace.size != sizeof(trace) || trace.version != thumbSurfaceProbeVersion || trace.reserved ||
        trace.sampleCount > thumbSurfaceCapacity ||
        (trace.capacityDrops && trace.sampleCount != thumbSurfaceCapacity)) return false;
    if (trace.startError && (trace.elapsedMs || trace.endError || trace.snapshotError)) return false;
    if (trace.startError || trace.snapshotError) {
        if (trace.sampleCount || trace.busyDrops || trace.capacityDrops) return false;
        for (const auto count : trace.stages)
            if (count) return false;
        for (const auto count : trace.cacheCounters)
            if (count) return false;
    }
    if (!validThumbCacheCounters(trace)) return false;
    auto remaining = thumbStage(trace, ThumbSurfaceStage::Accepted);
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& row = trace.samples[i];
        if (!row.calls || row.calls > remaining || !validThumbSurfaceIdentity(row.identity)) return false;
        for (DWORD j = 0; j < i; ++j)
            if (row.identity == trace.samples[j].identity) return false;
        remaining -= row.calls;
    }
    return remaining == trace.capacityDrops;
}

int thumbReplyError(const ThumbSurfaceTrace& trace, const ThumbCallResult& call)
{
    const auto worker = call.worker.exitCode;
    if (!call.received || !validThumbTrace(trace))
        return attachResult(AttachStatus::Ambiguous, call.transportError ? call.transportError : ERROR_INVALID_DATA, worker);
    if (trace.endError || trace.snapshotError)
        return attachResult(AttachStatus::Ambiguous, trace.endError ? trace.endError : trace.snapshotError, worker);
    if (call.transportError || !call.payloadReleased)
        return attachResult(AttachStatus::Error, call.transportError ? call.transportError : ERROR_INVALID_STATE, worker);
    if (trace.startError && worker != trace.startError)
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_DATA, worker);
    if (worker || trace.startError)
        return attachResult(AttachStatus::Error, ERROR_SUCCESS, worker ? worker : trace.startError);
    if (trace.elapsedMs < thumbSurfaceCaptureMs) return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE);
    return 0;
}

struct ThumbCoverage {
    std::array<unsigned, 3> pieces{};
    void add(const ThumbSurfaceIdentity& id) {
        const auto owner = id.scrollerTableRva == 0x3CD00 ? 0 : id.orientation ? 2 : 1;
        pieces[owner] |= thumbPiece(id.drawReturnRva);
    }
    bool complete() const {
        return (pieces[0] & thumbVerticalPieces) == thumbVerticalPieces &&
               (pieces[1] & thumbVerticalPieces) == thumbVerticalPieces &&
               (pieces[2] & thumbHorizontalPieces) == thumbHorizontalPieces;
    }
};

void printThumbTrace(const ThumbSurfaceTrace& trace, DWORD pid, ULONGLONG creation)
{
    std::printf("{\"status\":\"%s\",\"probeVersion\":%lu,\"pid\":%lu,\"creation\":\"%llu\","
                "\"elapsedMs\":%lu,\"iatUnchanged\":true,\"payloadReleased\":true,"
                "\"iatProtection\":%lu,\"busyDrops\":%llu,\"capacityDrops\":%llu,\"stages\":{",
                trace.sampleCount ? "captured" : "no_samples", trace.version, pid, creation, trace.elapsedMs,
                static_cast<DWORD>(PAGE_READONLY), trace.busyDrops, trace.capacityDrops);
    for (size_t i = 0; i < thumbStageNames.size(); ++i)
        std::printf("%s\"%s\":%llu", i ? "," : "", thumbStageNames[i], trace.stages[i]);
    std::fputs("},\"cacheDiagnostics\":{", stdout);
    for (size_t i = 0; i < thumbCacheNames.size(); ++i)
        std::printf("%s\"%s\":%llu", i ? "," : "", thumbCacheNames[i], trace.cacheCounters[i]);
    std::fputs("},\"samples\":[", stdout);
    ThumbCoverage coverage;
    for (DWORD i = 0; i < trace.sampleCount; ++i) {
        const auto& row = trace.samples[i];
        const auto& id = row.identity;
        coverage.add(id);
        std::printf("%s{\"calls\":%llu,\"windowTableRva\":%lu,\"scrollerTableRva\":%lu,"
                    "\"componentOffset\":%lu,\"drawReturnRva\":%lu,\"orientation\":%lu,\"hotId\":%lu,\"pressedId\":%lu,"
                    "\"cacheState\":%lu,\"enabled\":%lu,\"interactive\":%lu,\"rect\":[%ld,%ld,%ld,%ld],"
                    "\"thumbRect\":[%ld,%ld,%ld,%ld],\"surfaceType\":%ld,\"format\":%ld,\"width\":%ld,\"height\":%ld,"
                    "\"stride\":%ld,\"status\":%ld,\"deviceScale\":[%.17g,%.17g],\"deviceOffset\":[%.17g,%.17g]}",
                    i ? "," : "", row.calls, id.windowTableRva, id.scrollerTableRva, id.componentOffset,
                    id.drawReturnRva, id.orientation, id.hotId, id.pressedId, id.cacheState, id.enabled, id.interactive,
                    id.left, id.top, id.right, id.bottom, id.thumbLeft, id.thumbTop, id.thumbRight, id.thumbBottom,
                    id.surfaceType, id.format, id.width, id.height, id.stride, id.status,
                    id.deviceScaleX, id.deviceScaleY, id.deviceOffsetX, id.deviceOffsetY);
    }
    const bool clean = !trace.busyDrops && !trace.capacityDrops && !thumbStage(trace, ThumbSurfaceStage::ReadFailure) &&
        !thumbStage(trace, ThumbSurfaceStage::SurfaceError) && !thumbStage(trace, ThumbSurfaceStage::SurfaceType) &&
        !thumbStage(trace, ThumbSurfaceStage::InvalidMetrics);
    std::printf("],\"pieceCoverage\":{\"treeV\":%u,\"editorV\":%u,\"editorH\":%u},\"captureComplete\":%s}\n",
                coverage.pieces[0], coverage.pieces[1], coverage.pieces[2], clean && coverage.complete() ? "true" : "false");
}

DWORD checkThumbTarget(HANDLE process, DWORD pid, ULONGLONG creation, const DesignerWindow& window)
{
    if (const auto error = checkProcessLifetime(process, creation)) return error;
    BOOL debugged{};
    if (!CheckRemoteDebuggerPresent(process, &debugged)) return GetLastError();
    if (debugged) return ERROR_DEBUGGER_INACTIVE;
    if (!window.window || findDesignerMainWindow(pid) != window || IsIconic(window.window)) return ERROR_NOT_READY;
    return ERROR_SUCCESS;
}

int thumbAdmissionError(HANDLE process, ULONGLONG creation, DWORD error)
{
    if (error == ERROR_NOT_READY) return attachResult(AttachStatus::NotReady, error);
    if (error == ERROR_DEBUGGER_INACTIVE) return attachResult(AttachStatus::Conflict, error);
    return attachError(process, creation, error);
}

int thumbSurfaceToDesigner(DWORD pid, ULONGLONG creation, const Sha256Hex& expectedHash)
{
    const Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!static_cast<HANDLE>(query)) {
        const auto error = GetLastError();
        return attachResult(error == ERROR_INVALID_PARAMETER ? AttachStatus::Stale : AttachStatus::Error, error);
    }
    if (const auto error = checkProcessLifetime(query, creation)) return attachError(query, creation, error);
    std::wstring exe;
    if (const auto error = validateDesigner(query, pid, exe)) return attachError(query, creation, error);
    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    if (directory.empty()) return attachResult(AttachStatus::Error, ERROR_PATH_NOT_FOUND);
    const auto dll = (directory / thumbThemeName).wstring();
    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    if (!matchesSha256(dll, expectedHash.data())) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    constexpr DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
                             PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
    const Handle process(OpenProcess(rights, FALSE, pid));
    if (!static_cast<HANDLE>(process)) return attachError(query, creation, GetLastError());
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    std::wstring reopenedExe;
    if (const auto error = validateDesigner(process, pid, reopenedExe)) return attachError(process, creation, error);
    if (!pathEqual(exe, reopenedExe.c_str())) return attachResult(AttachStatus::Stale, ERROR_PROCESS_ABORTED);
    const auto window = findDesignerMainWindow(pid);
    if (const auto error = checkThumbTarget(process, pid, creation, window))
        return thumbAdmissionError(process, creation, error);

    ThumbModules before;
    if (const auto error = captureThumbModules(pid, before))
        return error == ERROR_ALREADY_EXISTS ? attachResult(AttachStatus::Conflict, error) :
                                               attachError(process, creation, error);
    const auto& mappedTheme = before[ThumbTheme];
    if (!mappedTheme.base) return attachResult(AttachStatus::NotReady, ERROR_MOD_NOT_FOUND);
    if (!thumbModulePath(mappedTheme, dll)) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (const auto error = validateThumbPlatformModules(before, matchesSha256))
        return attachResult(AttachStatus::Unsupported, error);
    AttachImage image(dll);
    const auto run = thumbMappedRun(process, image, mappedTheme);
    if (!run) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    if (const auto error = checkThumbTarget(process, pid, creation, window))
        return thumbAdmissionError(process, creation, error);
    const auto beforeHook = thumbHookPointer(process, before[ThumbGraphics], mappedTheme);
    if (!beforeHook) return attachResult(AttachStatus::Conflict, ERROR_INVALID_STATE);
    ThumbSurfaceTrace trace{};
    const auto call = callThumbSurfaceProbe(process, run, trace);
    if (call.worker.mayUsePayload) return attachResult(AttachStatus::Ambiguous, call.transportError);
    ThumbModules after;
    if (const auto error = checkThumbTarget(process, pid, creation, window))
        return attachResult(AttachStatus::Ambiguous, error, call.worker.exitCode);
    if (const auto error = captureThumbModules(pid, after))
        return attachResult(AttachStatus::Ambiguous, error, call.worker.exitCode);
    if (!sameThumbModules(before, after) || thumbMappedRun(process, image, after[ThumbTheme]) != run ||
        !thumbHookUnchanged(process, after, beforeHook))
        return attachResult(AttachStatus::Ambiguous, ERROR_INVALID_STATE, call.worker.exitCode);
    if (const auto error = thumbReplyError(trace, call)) return error;
    printThumbTrace(trace, pid, creation);
    return 0;
}

int thumbSurfaceCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    Sha256Hex hash{};
    if (argc != 5 || std::wstring_view(argv[1]) != L"--thumb-surface-probe" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation) || !thumbExpectedHash(argv[4], hash))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return thumbSurfaceToDesigner(pid, creation, hash);
}
} // namespace

#ifndef THUMB_SURFACE_LAUNCHER_TEST
int wmain(int argc, wchar_t** argv)
{
    try { return thumbSurfaceCommand(argc, argv); }
    catch (const std::bad_alloc&) { return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY); }
    catch (const std::exception&) { return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE); }
}
#endif
