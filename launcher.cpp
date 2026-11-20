#include "platform.hpp"
#include "attach_process.hpp"
#ifdef THEME_TOOLTIP_TRACE
#include "tooltip_trace.hpp"
#elif defined(THEME_FORM_LINE_TRACE)
#include "form_line_trace.hpp"
#elif defined(THEME_OUTLINE_TRACE)
#include "outline_trace.hpp"
#elif defined(THEME_GRID_TRACE)
#include "grid_trace.hpp"
#elif defined(THEME_SEARCH_RESULTS_TRACE)
#include "search_results_trace.hpp"
#endif

#if defined(THEME_OUTLINE_TRACE) || defined(THEME_GRID_TRACE)
#define THEME_SPREADSHEET_TRACE
#endif
#if defined(THEME_FORM_LINE_TRACE) || defined(THEME_SPREADSHEET_TRACE)
#define THEME_DOCUMENT_TRACE
#endif
#if defined(THEME_TOOLTIP_TRACE) || defined(THEME_DOCUMENT_TRACE) || defined(THEME_SEARCH_RESULTS_TRACE)
#define THEME_HAS_TRACE
#endif
#if !defined(THEME_HAS_TRACE) && !defined(THEME_BENCHMARK)
#include "list_identity.hpp"
#include <winternl.h>
#include <algorithm>
#include <vector>
#endif

#include <tlhelp32.h>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string_view>
#ifdef THEME_BENCHMARK
#include <chrono>
#include <psapi.h>
#endif

static_assert(sizeof(void*) == 8);

namespace {

class Handle {
    HANDLE value_;
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value_; }
};

// The target module may be mapped at a different address than in the launcher.
ULONG_PTR remoteModule(DWORD pid, const std::wstring& path)
{
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    MODULEENTRY32W item{};
    item.dwSize = sizeof(item);
    for (auto found = Module32FirstW(snapshot, &item); found;
         found = Module32NextW(snapshot, &item)) {
        if (_wcsicmp(item.szExePath, path.c_str()) == 0)
            return reinterpret_cast<ULONG_PTR>(item.modBaseAddr);
    }
    return 0;
}

DWORD remoteCall(HANDLE child, ULONG_PTR address, LPVOID arg, DWORD& result)
{
    const Handle worker(CreateRemoteThread(child, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), arg, 0, nullptr));
    if (!static_cast<HANDLE>(worker)) return GetLastError();
    if (WaitForSingleObject(worker, 30000) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
    // A started call with an unknown outcome must not be retried as a transport failure.
    return GetExitCodeThread(worker, &result) ? ERROR_SUCCESS : WAIT_TIMEOUT;
}

DWORD themeExportRva(const std::wstring& dll, const char* exportName, ULONG_PTR& exportRva)
{
    const auto image = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return GetLastError();
    const auto exportAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(image, exportName));
    FreeLibrary(image);
    if (!exportAddress) return ERROR_PROC_NOT_FOUND;
    exportRva = exportAddress - reinterpret_cast<ULONG_PTR>(image);
    return ERROR_SUCCESS;
}

DWORD loadTheme(HANDLE child, DWORD pid, const std::wstring& dll, ULONG_PTR& setter,
                const char* exportName)
{
    ULONG_PTR exportRva{};
    const DWORD exportError = themeExportRva(dll, exportName, exportRva);
    if (exportError) return exportError;

    const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE localOwner{};
    constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!load || !GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(load), &localOwner))
        return ERROR_PROC_NOT_FOUND;
    const auto remoteOwner = remoteModule(pid, modulePath(localOwner));
    if (!remoteOwner) return ERROR_MOD_NOT_FOUND;
    const auto loadRva = reinterpret_cast<ULONG_PTR>(load) -
                         reinterpret_cast<ULONG_PTR>(localOwner);

    const auto size = (dll.size() + 1) * sizeof(wchar_t);
    const auto path = VirtualAllocEx(child, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!path) return GetLastError();
    DWORD result{};
    DWORD error = WriteProcessMemory(child, path, dll.c_str(), size, nullptr) ?
        remoteCall(child, remoteOwner + loadRva, path, result) : GetLastError();
    if (error != WAIT_TIMEOUT) VirtualFreeEx(child, path, 0, MEM_RELEASE);
    if (error) return error;
    // The thread exit value is 32-bit; find the full 64-bit module base.
    const auto base = remoteModule(pid, dll);
    if (!base) return ERROR_DLL_INIT_FAILED;
    setter = base + exportRva;
    return ERROR_SUCCESS;
}

#ifdef THEME_TOOLTIP_TRACE
using TraceResult = TooltipTrace;
constexpr char traceExport[] = "ReadTooltipTrace";
#elif defined(THEME_FORM_LINE_TRACE)
using TraceResult = FormLineTrace;
constexpr char traceExport[] = "ReadFormLineTrace";
#elif defined(THEME_OUTLINE_TRACE)
using TraceResult = OutlineTrace;
constexpr char traceExport[] = "ReadOutlineTraceV2";
#elif defined(THEME_GRID_TRACE)
using TraceResult = GridTrace;
constexpr char traceExport[] = "ReadGridTraceV4";
#elif defined(THEME_SEARCH_RESULTS_TRACE)
using TraceResult = SearchResultsTrace;
constexpr char traceExport[] = "ReadSearchResultsTraceV6";
#endif

#ifdef THEME_HAS_TRACE
DWORD readTrace(HANDLE child, DWORD pid, const std::wstring& dll)
{
    const auto base = remoteModule(pid, dll);
    if (!base) return ERROR_MOD_NOT_FOUND;
    ULONG_PTR exportRva{};
    DWORD error = themeExportRva(dll, traceExport, exportRva);
    if (error) return error;

    TraceResult trace{};
    const auto output = VirtualAllocEx(child, nullptr, sizeof(trace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!output) return GetLastError();
    DWORD result{};
    error = remoteCall(child, base + exportRva, output, result);
    if (error == WAIT_TIMEOUT) return error;
    if (!error) error = result;
    if (!error && !ReadProcessMemory(child, output, &trace, sizeof(trace), nullptr)) error = GetLastError();
    VirtualFreeEx(child, output, 0, MEM_RELEASE);
    if (error) return error;

#ifdef THEME_SEARCH_RESULTS_TRACE
    std::puts("Search grid property 4 (calls since previous read):");
    std::printf("  Cached color: %lu\n  Original color: %lu\n  Unexpected color source: %lu\n"
                "  Other grid class: %lu\n",
                trace.cachedColorCalls, trace.originalColorCalls,
                trace.unexpectedColorCalls, trace.otherGridCalls);
    std::puts("Second cached-color read (calls since previous read):");
    std::printf("  Matching grid class: %lu\n  Other grid class: %lu\n",
                trace.cachedColorCopies, trace.otherGridColorCopies);
    std::puts("White palette writes across all receivers (calls since previous read):");
    for (size_t i = 0; i < trace.whiteWrites.size(); ++i)
        std::printf("  frame+0x%llX: %lu\n", searchGridWhiteWriteRvas[i], trace.whiteWrites[i]);
    std::printf("Factory under results loader: standalone=%lu aggregated=%lu, outside loader=%lu\n",
                trace.resultsStandaloneCreations, trace.resultsAggregatedCreations, trace.otherCreations);
    std::printf("Background fills: last search-load candidate=%lu other=%lu\n",
                trace.resultsBackgrounds, trace.otherBackgrounds);
    std::printf("Search background replacements in Cairo: %lu\n", trace.replacedCairoFills);
    std::printf("Search row replacements in Cairo: %lu\n", trace.replacedRowFills);
    std::printf("Search RGBA replacements: text background=%lu text=%lu\n",
                trace.replacedTextBackgrounds, trace.replacedTexts);
#elif defined(THEME_GRID_TRACE)
    std::puts("Spreadsheet grid rectangles (x, y, width, height, calls since previous read):");
    for (const auto& sample : trace.rectangles) {
        const auto& rect = sample.rectangle;
        if (sample.calls) std::printf("  moxelui+0x%lX (%g, %g, %g, %g) %lu\n",
            sample.rendererReturn, rect[0], rect[1], rect[2], rect[3], sample.calls);
    }
    std::puts("Spreadsheet grid solid colors (RGBA, calls since previous read):");
    for (const auto& sample : trace.colors) {
        if (sample.calls) std::printf("  %s moxelui+0x%lX #%08lX %lu\n",
            sample.mask ? "RGBA mask" : "RGB", sample.rendererReturn, sample.rgba, sample.calls);
    }
    std::printf("  Other rectangles: %lu; other colors: %lu\n", trace.otherRectangles, trace.otherColors);
    std::puts("moxelui code-address candidates for rectangles rejected by the renderer check:");
    for (const auto& sample : trace.rendererCandidates) {
        if (!sample.calls) continue;
        std::printf("  stack+0x%lX moxelui+0x%lX total=%lu one-pixel=%lu",
            sample.stackOffset, sample.rva, sample.calls, sample.onePixel);
        if (sample.moxelCaller) std::printf(" moxel+0x%lX", sample.moxelCaller);
        std::putchar('\n');
    }
    std::printf("  Rectangles without candidates: %lu; other candidate observations: %lu\n",
                trace.noRenderer, trace.otherRenderers);
#elif defined(THEME_OUTLINE_TRACE)
    std::puts("Spreadsheet outline samples (coordinates, colors and calls since previous read):");
    for (const auto& sample : trace.outlines) {
        if (sample.calls) std::printf("  %s moxelui+0x%lX (%ld, %ld, %ld, %ld) input=0x%08lX #%08lX %lu\n",
            sample.mask ? "RGBA mask" : "RGB", sample.rendererReturn,
            sample.rect.left, sample.rect.top, sample.rect.right, sample.rect.bottom,
            sample.inputColor, sample.rgba, sample.calls);
    }
#elif defined(THEME_FORM_LINE_TRACE)
    std::puts("frmcore line samples (RGBA, logical coordinates, calls since previous read):");
    for (const auto& line : trace.lines) {
        if (line.calls) std::printf("  #%08lX (%ld, %ld) -> (%ld, %ld) %lu\n",
            line.rgba, line.x1, line.y1, line.x2, line.y2, line.calls);
    }
#else
    std::puts("Expanded tooltip RGBA (counts since previous read):");
    for (const auto& color : trace.colors) {
        if (color.calls) std::printf("  #%08lX %lu\n", color.rgba, color.calls);
    }
#endif
#if !defined(THEME_GRID_TRACE) && !defined(THEME_SEARCH_RESULTS_TRACE)
    std::printf("  Other calls: %lu\n", trace.otherCalls);
#endif
#ifdef THEME_SPREADSHEET_TRACE
    constexpr const char* hookNames[] = {"RGB", "RGBA", "Rectangle"};
    std::puts("Cairo IAT entries and outcomes (calls since previous read):");
    for (size_t i = 0; i < trace.stages.size(); ++i) {
        const auto& [caller, bounds, graphics, renderer, invalid, match] = trace.stages[i];
        const auto total = caller + bounds + graphics + renderer + invalid + match;
        std::printf("  %s total=%llu caller=%llu bounds=%llu graphics=%llu renderer=%llu invalid=%llu match=%llu\n",
                    hookNames[i], total, caller, bounds, graphics, renderer, invalid, match);
    }
#endif
    std::fflush(stdout);
    return ERROR_SUCCESS;
}
#endif

std::wstring quoteCommandArgument(std::wstring_view value)
{
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool serverRef(const std::wstring& target)
{
    if (std::filesystem::path(target).is_absolute() && target.find_first_of(L"\"\r\n") == std::wstring::npos)
        return std::filesystem::is_regular_file(std::filesystem::path(target) / L"1Cv8.1CD");
    const auto slash = target.find(L'\\');
    return slash != std::wstring::npos && slash != 0 && slash + 1 < target.size() &&
           target.find(L'\\', slash + 1) == std::wstring::npos &&
           target.find_first_of(L"\"\r\n/") == std::wstring::npos;
}

bool parseProfile(std::wstring_view input, ThemeProfile& profile)
{
    const auto separator = input.find(L':');
    if (separator == std::wstring_view::npos || separator == 0 || separator > 6)
        return false;
    DWORD zones{};
    for (const wchar_t digit : input.substr(0, separator)) {
        if (digit < L'0' || digit > L'9') return false;
        zones = zones * 10 + (digit - L'0');
    }
    if (zones > themeZoneMask) return false;
    input.remove_prefix(separator + 1);
    if ((input.size() + 1) % 7 != 0) return false;
    const size_t colorCount = (input.size() + 1) / 7;
    if (colorCount != paletteColorCount &&
        !(colorCount == scrollerThumbBackground && zones < enableScrollerThumb) &&
        !(colorCount == scrollerTrackBackground && zones < enableScrollerPng) &&
        !(colorCount == scrollerCornerBackground && zones < enableScrollerCorner) &&
        !(colorCount == dockSplitterFill && zones < enableDockSplitter) &&
        !(colorCount == panelBorder && zones < enableDockSplitter) &&
        !(colorCount == panelHeaderBackground && zones < enablePanelHeaders) &&
        !(colorCount == searchResultsBackground && zones < enableSearchResults) &&
        !(colorCount == toolbarBorder && zones < enableToolbarBorder) &&
        !(colorCount == toolbarBandBackground && zones < enableToolbarBand) &&
        !(colorCount == popupMenuBackground && zones < enablePopupMenu))
        return false;
    ThemeProfile parsedProfile;
    parsedProfile.zones = zones;
    for (size_t i = 0; i < colorCount; ++i) {
        char hex[6];
        for (size_t digit = 0; digit < sizeof(hex); ++digit) {
            if (input[digit] > 127) return false;
            hex[digit] = static_cast<char>(input[digit]);
        }
        const auto end = hex + sizeof(hex);
        const auto parsed = std::from_chars(hex, end, parsedProfile.colors[i], 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
        if (i + 1 < colorCount) {
            if (input[sizeof(hex)] != L',') return false;
            input.remove_prefix(sizeof(hex) + 1);
        }
    }
    profile = parsedProfile;
    return true;
}

#if !defined(THEME_HAS_TRACE) && !defined(THEME_BENCHMARK)
DWORD lockLaunchedTheme(HANDLE child, DWORD pid, ThemeProcessLock& targetLock)
{
    ULONGLONG creation{};
    if (const auto error = readProcessCreation(child, creation)) return error;
    const auto lockResult = targetLock.acquire(pid, creation);
    if (lockResult == WAIT_ABANDONED) return themeStateUncertain;
    if (lockResult == WAIT_TIMEOUT) return ERROR_RETRY;
    if (lockResult) return lockResult;
    return checkProcessLifetime(child, creation);
}

DWORD loadLaunchedTheme(HANDLE child, DWORD pid, const std::wstring& dll, ULONG_PTR& setter,
                        const char* exportName);
#else
DWORD lockLaunchedTheme(HANDLE, DWORD, ThemeProcessLock&) { return ERROR_SUCCESS; }
constexpr auto loadLaunchedTheme = loadTheme;
#endif

int applyLaunchProfile(HANDLE child, DWORD pid, const std::wstring& dll, const ThemeProfile& profile, bool enterpriseMode = false)
{
    std::printf("Started 1C client PID %lu.\n", pid);
    std::fflush(stdout);
    if (!profile.zones) return 0;

    ULONG_PTR setter{};
    LPVOID payload{};
    DWORD error = ERROR_TIMEOUT;
    DWORD result{};
    const auto mainWindow = [pid, enterpriseMode]() {
        if (!enterpriseMode) return findDesignerMainWindow(pid).window;
        struct Search { DWORD pid; HWND window; } search{pid, nullptr};
        EnumWindows([](HWND window, LPARAM value) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(value);
            DWORD owner{}; GetWindowThreadProcessId(window, &owner);
            if (owner == search.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
                search.window = window; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.window;
    };
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 180000) {
        if (WaitForSingleObject(child, 0) != WAIT_TIMEOUT) {
            error = ERROR_PROCESS_ABORTED;
            break;
        }
        const auto target = mainWindow();
        if (target) {
            ThemeProcessLock targetLock;
            error = lockLaunchedTheme(child, pid, targetLock);
            if (error == ERROR_RETRY) error = ERROR_MOD_NOT_FOUND;
            if (!error && !mainWindow()) error = ERROR_MOD_NOT_FOUND;
            if (!error && !setter) error = enterpriseMode ? loadTheme(child, pid, dll, setter, "SetThemeProfile") :
                loadLaunchedTheme(child, pid, dll, setter, "SetThemeProfile");
            if (error && error != ERROR_MOD_NOT_FOUND && error != ERROR_NOT_READY) break;
            if (!error && !payload) {
                payload = VirtualAllocEx(child, nullptr, sizeof(profile), MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
                if (!payload || !WriteProcessMemory(child, payload, &profile, sizeof(profile), nullptr)) {
                    error = GetLastError();
                    break;
                }
            }
            if (!error && payload) {
                error = remoteCall(child, setter, payload, result);
                if (error) break;
                if (result != ERROR_MOD_NOT_FOUND && result != ERROR_NOT_READY &&
                    result != static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND))) {
                    if (!result) {
                        const auto window = mainWindow();
                        if (window) RedrawWindow(window, nullptr, nullptr,
                            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                    }
                    break;
                }
            }
        }
        if (WaitForSingleObject(child, 250) != WAIT_TIMEOUT) {
            error = ERROR_PROCESS_ABORTED;
            break;
        }
        error = ERROR_TIMEOUT;
    }
    if (payload && error != WAIT_TIMEOUT) VirtualFreeEx(child, payload, 0, MEM_RELEASE);
    if (error == ERROR_TIMEOUT)
        std::fputs("Configurator was not ready within 3 minutes. Check its startup window.\n", stderr);
    if (error || result) {
        std::fprintf(stderr, "Theme profile failed: transport=%lu dll=0x%08lX\n", error, result);
        return 1;
    }
    std::puts("Theme profile applied.");
    return 0;
}

#if !defined(THEME_HAS_TRACE) && !defined(THEME_BENCHMARK)
bool pathEqual(const std::wstring& left, const wchar_t* right)
{
    return CompareStringOrdinal(left.c_str(), -1, right, -1, TRUE) == CSTR_EQUAL;
}

enum class AttachStatus { Applied, Disabled, Error, InvalidArgument, NotReady, Stale,
                          Unsupported, Conflict, Ambiguous };

int attachResult(AttachStatus status, DWORD win32 = ERROR_SUCCESS, DWORD dll = ERROR_SUCCESS)
{
    constexpr struct { const char* token; int exitCode; } results[] = {
        {"applied", 0}, {"disabled", 0}, {"error", 1}, {"invalid_argument", 2},
        {"not_ready", 3}, {"stale", 4}, {"unsupported", 5}, {"conflict", 6}, {"ambiguous", 7},
    };
    const auto result = results[static_cast<size_t>(status)];
    std::printf("{\"status\":\"%s\",\"win32\":%lu,\"dll\":%lu}\n", result.token, win32, dll);
    return result.exitCode;
}

template<class T>
bool parseDecimal(std::wstring_view input, T& result)
{
    if (input.empty()) return false;
    result = 0;
    for (const auto ch : input) {
        if (ch < L'0' || ch > L'9') return false;
        const auto digit = static_cast<T>(ch - L'0');
        if (result > (std::numeric_limits<T>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return result != 0;
}

DWORD readProcessCommandLine(HANDLE process, std::wstring& commandLine)
{
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return ERROR_MOD_NOT_FOUND;
    using Query = NTSTATUS (NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    using ToError = ULONG (NTAPI*)(NTSTATUS);
    const auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    const auto toError = reinterpret_cast<ToError>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (!query || !toError) return ERROR_NOT_SUPPORTED;

    // ProcessCommandLineInformation writes one counted string into this caller's buffer.
    constexpr ULONG capacity = 128 * 1024;
    std::vector<ULONG_PTR> buffer(capacity / sizeof(ULONG_PTR));
    ULONG returned{};
    const auto status = query(process, static_cast<PROCESSINFOCLASS>(60), buffer.data(),
                              capacity, &returned);
    if (status < 0) return toError(status);
    if (returned < sizeof(UNICODE_STRING) || returned > capacity) return ERROR_INVALID_DATA;
    UNICODE_STRING argument{};
    std::memcpy(&argument, buffer.data(), sizeof(argument));
    const ULONG_PTR start = reinterpret_cast<ULONG_PTR>(buffer.data());
    const ULONG_PTR end = start + returned;
    const ULONG_PTR text = reinterpret_cast<ULONG_PTR>(argument.Buffer);
    if (!argument.Length || argument.Length % sizeof(wchar_t) ||
        argument.Length > argument.MaximumLength || text % alignof(wchar_t) ||
        text < start + sizeof(argument) || text > end || argument.Length > end - text)
        return ERROR_INVALID_DATA;
    commandLine.assign(argument.Buffer, argument.Length / sizeof(wchar_t));
    return commandLine.find(L'\0') == std::wstring::npos ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

DWORD validateDesigner(HANDLE process, DWORD pid, std::wstring& path, bool enterprise = false)
{
    DWORD ourSession{}, theirSession{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ourSession) ||
        !ProcessIdToSessionId(pid, &theirSession)) return GetLastError();
    if (ourSession != theirSession) return ERROR_NOT_SUPPORTED;
    USHORT processMachine{}, nativeMachine{};
    if (!IsWow64Process2(process, &processMachine, &nativeMachine)) return GetLastError();
    if (processMachine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64)
        return ERROR_NOT_SUPPORTED;
    path.resize(32768);
    auto length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return GetLastError();
    path.resize(length);
    const auto name = path.find_last_of(L"\\/");
    if (name == std::wstring::npos) return ERROR_REVISION_MISMATCH;
    const auto executable = path.c_str()+name+1;
    const bool thin = enterprise && CompareStringOrdinal(executable,-1,L"1cv8c.exe",-1,TRUE) == CSTR_EQUAL;
    const bool thick = CompareStringOrdinal(executable,-1,L"1cv8.exe",-1,TRUE) == CSTR_EQUAL;
    if ((!thin && !thick) || !matchesSha256(path,thin ? thinClientHash : exeHash))
        return ERROR_REVISION_MISMATCH;
    std::wstring commandLine;
    const auto error = readProcessCommandLine(process, commandLine);
    if (error) return error;
    return isOneCCommandLine(commandLine.c_str(),enterprise ? L"ENTERPRISE" : L"DESIGNER",thin)
        ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}

struct AttachModules {
    ULONG_PTR ownBase{};
    DWORD ownSize{};
    bool competingDll{};
    std::wstring frame;
    std::wstring graphics;
    std::wstring frontend;
    std::wstring wbase;
    std::wstring cairo;
};

template<typename HashCheck>
DWORD validateAttachModules(const AttachModules& modules, HashCheck matches)
{
    if (modules.frame.empty() || modules.graphics.empty() || modules.frontend.empty() || modules.wbase.empty() ||
        modules.cairo.empty())
        return ERROR_MOD_NOT_FOUND;
    if (!matches(modules.frame, frameHash) || !matches(modules.graphics, graphicsHash) ||
        !matches(modules.frontend, listFrontendHash) || !matches(modules.wbase, wbaseHash) ||
        !matches(modules.cairo, cairoHash))
        return ERROR_REVISION_MISMATCH;
    return ERROR_SUCCESS;
}

DWORD snapshotAttachModules(DWORD pid, const std::wstring& dll, AttachModules& modules,
                            const wchar_t* moduleName = L"tree-theme.dll")
{
    modules = {};
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    if (static_cast<HANDLE>(snapshot) == INVALID_HANDLE_VALUE) return GetLastError();
    MODULEENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Module32FirstW(snapshot, &item)) return GetLastError();
    do {
        if (pathEqual(moduleName, item.szModule)) {
            if (pathEqual(dll, item.szExePath)) {
                modules.ownBase = reinterpret_cast<ULONG_PTR>(item.modBaseAddr);
                modules.ownSize = item.modBaseSize;
            } else {
                modules.competingDll = true;
            }
        } else if (pathEqual(L"frame.dll", item.szModule)) {
            modules.frame = item.szExePath;
        } else if (pathEqual(L"grphcs.dll", item.szModule)) {
            modules.graphics = item.szExePath;
        } else if (pathEqual(L"frntend.dll", item.szModule)) {
            modules.frontend = item.szExePath;
        } else if (pathEqual(L"wbase83.dll", item.szModule)) {
            modules.wbase = item.szExePath;
        } else if (pathEqual(L"cairo.dll", item.szModule)) {
            modules.cairo = item.szExePath;
        }
    } while (Module32NextW(snapshot, &item));
    const auto error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}

class AttachImage {
    HMODULE image_;
    DWORD size_{};
    DWORD headers_{};
    IMAGE_DATA_DIRECTORY exports_{};
    ULONG_PTR setter_{};
public:
    explicit AttachImage(const std::wstring& path)
        : image_(LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {}
    ~AttachImage() { if (image_) FreeLibrary(image_); }
    AttachImage(const AttachImage&) = delete;
    AttachImage& operator=(const AttachImage&) = delete;

    bool validate(const char* exportName = "SetThemeProfile",
                  const char* abiExport = themeProfileAttachExport)
    {
        if (!image_) return false;
        const auto base = reinterpret_cast<ULONG_PTR>(image_);
        const auto& dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
            dos.e_lfanew > 4096 - sizeof(IMAGE_NT_HEADERS64)) return false;
        const auto& nt = *reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos.e_lfanew);
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        const auto& optional = nt.OptionalHeader;
        size_ = optional.SizeOfImage;
        headers_ = optional.SizeOfHeaders;
        exports_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!headers_ || headers_ > 65536 || headers_ > size_ ||
            exports_.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || exports_.Size > 65536 ||
            !exports_.VirtualAddress || exports_.VirtualAddress >= size_ ||
            exports_.Size > size_ - exports_.VirtualAddress) return false;
        const auto executableExport = [&](const char* name) {
            const auto code = reinterpret_cast<ULONG_PTR>(GetProcAddress(image_, name));
            MEMORY_BASIC_INFORMATION region{};
            if (code < base || code - base >= size_ ||
                !VirtualQuery(reinterpret_cast<LPCVOID>(code), &region, sizeof(region)) ||
                region.AllocationBase != image_ || !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return ULONG_PTR{0};
            return code - base;
        };
        setter_ = executableExport(exportName);
        return setter_ && executableExport(abiExport);
    }

    ULONG_PTR remoteSetter(HANDLE process, const AttachModules& modules) const
    {
        if (!modules.ownBase || modules.ownSize != size_) return 0;
        const auto base = reinterpret_cast<ULONG_PTR>(image_);
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(modules.ownBase + setter_),
                            &region, sizeof(region)) ||
            reinterpret_cast<ULONG_PTR>(region.AllocationBase) != modules.ownBase ||
            region.Type != MEM_IMAGE || !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        std::vector<BYTE> bytes((std::max)(headers_, exports_.Size));
        const auto same = [&](DWORD offset, DWORD count) {
            SIZE_T read{};
            return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(modules.ownBase + offset),
                                     bytes.data(), count, &read) && read == count &&
                   std::memcmp(bytes.data(), reinterpret_cast<const void*>(base + offset), count) == 0;
        };
        return same(0, headers_) && same(exports_.VirtualAddress, exports_.Size) ?
            modules.ownBase + setter_ : 0;
    }
};

DWORD loadLaunchedTheme(HANDLE child, DWORD pid, const std::wstring& dll, ULONG_PTR& setter,
                        const char* exportName)
{
    if (!findDesignerMainWindow(pid).window) return ERROR_NOT_READY;
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules)) return error;
    if (modules.competingDll) return ERROR_ALREADY_EXISTS;
    if (const auto error = validateAttachModules(modules, matchesSha256)) return error;
    if (!modules.ownBase) return loadTheme(child, pid, dll, setter, exportName);
    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE) return GetLastError();
    AttachImage image(dll);
    if (!image.validate(exportName)) return ERROR_REVISION_MISMATCH;
    setter = image.remoteSetter(child, modules);
    return setter ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH;
}

int attachError(HANDLE process, ULONGLONG creation, DWORD error)
{
    if (checkProcessLifetime(process, creation) == ERROR_PROCESS_ABORTED ||
        error == ERROR_PROCESS_ABORTED) return attachResult(AttachStatus::Stale, ERROR_PROCESS_ABORTED);
    if (error == ERROR_BAD_LENGTH || error == ERROR_NO_MORE_FILES || error == ERROR_MOD_NOT_FOUND)
        return attachResult(AttachStatus::NotReady, error);
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_REVISION_MISMATCH)
        return attachResult(AttachStatus::Unsupported, error);
    return attachResult(AttachStatus::Error, error);
}

HWND enterpriseMainWindow(DWORD pid)
{
    struct Search { DWORD pid; HWND window; } search{pid,nullptr};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& search=*reinterpret_cast<Search*>(data);
        DWORD owner{};
        GetWindowThreadProcessId(window,&owner);
        RECT bounds{};
        if (owner != search.pid || !IsWindowVisible(window) || GetWindow(window,GW_OWNER) ||
            (GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW) ||
            !GetClientRect(window,&bounds) || bounds.right<300 || bounds.bottom<180) return TRUE;
        search.window=window;
        return FALSE;
    },reinterpret_cast<LPARAM>(&search));
    return search.window;
}

int attachToDesigner(DWORD pid, ULONGLONG creation, const ThemeProfile& profile, bool enterprise = false)
{
    const Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!static_cast<HANDLE>(query)) {
        const auto error = GetLastError();
        return attachResult(error == ERROR_INVALID_PARAMETER ? AttachStatus::Stale :
                            AttachStatus::Error, error);
    }
    if (const auto error = checkProcessLifetime(query, creation)) return attachError(query, creation, error);
    std::wstring exe;
    if (const auto error = validateDesigner(query, pid, exe, enterprise)) return attachError(query, creation, error);

    ThemeProcessLock targetLock;
    const auto lockResult = targetLock.acquire(pid, creation);
    if (lockResult == WAIT_ABANDONED) return attachResult(AttachStatus::Ambiguous, lockResult);
    if (lockResult == WAIT_TIMEOUT) return attachResult(AttachStatus::NotReady, lockResult);
    if (lockResult) return attachError(query, creation, lockResult);

    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    if (directory.empty()) return attachResult(AttachStatus::Error, ERROR_PATH_NOT_FOUND);
    const auto moduleName=enterprise ? L"enterprise-theme.dll" : L"tree-theme.dll";
    const auto dll = (directory / moduleName).wstring();
    const auto mainWindow=[=] {return enterprise ? enterpriseMainWindow(pid) : findDesignerMainWindow(pid).window;};
    AttachModules modules;
    if (const auto error = snapshotAttachModules(pid, dll, modules,moduleName)) return attachError(query, creation, error);
    if (const auto error = checkProcessLifetime(query, creation)) return attachError(query, creation, error);
    if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (!profile.zones && !modules.ownBase) return attachResult(AttachStatus::Disabled);
    if (profile.zones && !mainWindow())
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);

    // Retain read access without sharing replacement or writes through validation and injection.
    const Handle imageFile(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(imageFile) == INVALID_HANDLE_VALUE)
        return attachResult(AttachStatus::Unsupported, GetLastError());
    AttachImage image(dll);
    if (!image.validate("SetThemeProfile",enterprise ? "EnterpriseThemeProfileAttachV11" : themeProfileAttachExport))
        return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);

    constexpr DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
                             PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
    const Handle process(OpenProcess(rights, FALSE, pid));
    if (!static_cast<HANDLE>(process)) return attachError(query, creation, GetLastError());
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    std::wstring reopenedExe;
    if (const auto error = validateDesigner(process, pid, reopenedExe,enterprise)) return attachError(process, creation, error);
    if (!pathEqual(exe, reopenedExe.c_str())) return attachResult(AttachStatus::Stale, ERROR_PROCESS_ABORTED);
    if (const auto error = snapshotAttachModules(pid, dll, modules,moduleName)) return attachError(process, creation, error);
    if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    if (!profile.zones && !modules.ownBase) return attachResult(AttachStatus::Disabled);
    const auto moduleValidation=enterprise
        ? (modules.graphics.empty() || modules.cairo.empty() ? ERROR_MOD_NOT_FOUND
           : matchesSha256(modules.graphics,graphicsHash) && matchesSha256(modules.cairo,cairoHash)
             ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH)
        : validateAttachModules(modules,matchesSha256);
    if (const auto error = moduleValidation)
        return error == ERROR_REVISION_MISMATCH ? attachResult(AttachStatus::Unsupported, error) :
                                               attachError(process, creation, error);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    if (profile.zones && !mainWindow())
        return attachResult(AttachStatus::NotReady, ERROR_NOT_READY);

    if (!modules.ownBase) {
        ULONG_PTR unusedSetter{};
        const auto loadError = loadTheme(process, pid, dll, unusedSetter, "SetThemeProfile");
        if (loadError == WAIT_TIMEOUT || loadError == ERROR_DLL_INIT_FAILED)
            return attachResult(AttachStatus::Ambiguous, loadError);
        if (loadError) return attachError(process, creation, loadError);
        if (const auto error = snapshotAttachModules(pid, dll, modules,moduleName))
            return attachResult(AttachStatus::Ambiguous, error);
        if (modules.competingDll) return attachResult(AttachStatus::Conflict, ERROR_ALREADY_EXISTS);
    }
    const auto setter = image.remoteSetter(process, modules);
    if (!setter) return attachResult(AttachStatus::Unsupported, ERROR_REVISION_MISMATCH);
    if (const auto error = checkProcessLifetime(process, creation)) return attachError(process, creation, error);
    const auto payload = VirtualAllocEx(process, nullptr, sizeof(profile), MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
    if (!payload) return attachError(process, creation, GetLastError());
    DWORD result{};
    const DWORD error = WriteProcessMemory(process, payload, &profile, sizeof(profile), nullptr) ?
        remoteCall(process, setter, payload, result) : GetLastError();
    if (error != WAIT_TIMEOUT) VirtualFreeEx(process, payload, 0, MEM_RELEASE);
    if (error == WAIT_TIMEOUT) return attachResult(AttachStatus::Ambiguous, error);
    if (error) return attachError(process, creation, error);
    if (result == themeStateUncertain) return attachResult(AttachStatus::Ambiguous, ERROR_SUCCESS, result);
    if (result == ERROR_MOD_NOT_FOUND || result == ERROR_NOT_READY ||
        result == static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)))
        return attachResult(AttachStatus::NotReady, ERROR_SUCCESS, result);
    if (result == ERROR_NOT_SUPPORTED || result == ERROR_REVISION_MISMATCH)
        return attachResult(AttachStatus::Unsupported, ERROR_SUCCESS, result);
    if (enterprise && result == ERROR_ALREADY_ASSIGNED) return attachResult(AttachStatus::Applied);
    if (result) return attachResult(AttachStatus::Error, ERROR_SUCCESS, result);
    if (const auto lifetimeError = checkProcessLifetime(process, creation))
        return attachError(process, creation, lifetimeError);
    const auto target = mainWindow();
    if (target && !checkProcessLifetime(process, creation))
        RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return attachResult(profile.zones ? AttachStatus::Applied : AttachStatus::Disabled);
}

int attachCommand(int argc, wchar_t** argv)
{
    DWORD pid{};
    ULONGLONG creation{};
    ThemeProfile profile;
    if (argc != 6 || std::wstring_view(argv[4]) != L"--palette" ||
        !parseDecimal(argv[2], pid) || !parseDecimal(argv[3], creation) ||
        !parseProfile(argv[5], profile))
        return attachResult(AttachStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
    return attachToDesigner(pid, creation, profile, std::wstring_view(argv[1]) == L"--attach-enterprise");
}
#endif

#ifdef THEME_BENCHMARK
struct BenchmarkWindow {
    DWORD pid;
    HWND window;
    size_t count;
};

BOOL CALLBACK findBenchmarkWindow(HWND window, LPARAM param)
{
    auto& target = *reinterpret_cast<BenchmarkWindow*>(param);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid == target.pid && IsWindowVisible(window) && !IsIconic(window) &&
        !GetWindow(window, GW_OWNER) && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
        target.window = window;
        ++target.count;
    }
    return TRUE;
}

ULONGLONG cpuTicks(const FILETIME& time)
{
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool benchmark(HANDLE child, DWORD pid)
{
    BenchmarkWindow target{pid, nullptr, 0};
    if (!EnumWindows(findBenchmarkWindow, reinterpret_cast<LPARAM>(&target)) || target.count != 1)
        return false;
    constexpr UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN;
    for (int i = 0; i < 10; ++i)
        if (!RedrawWindow(target.window, nullptr, nullptr, flags)) return false;

    FILETIME created{}, exited{}, kernelBefore{}, userBefore{}, kernelAfter{}, userAfter{};
    if (!GetProcessTimes(child, &created, &exited, &kernelBefore, &userBefore)) return false;
    const auto start = std::chrono::steady_clock::now();
    constexpr int cycles = 300;
    for (int i = 0; i < cycles; ++i)
        if (!RedrawWindow(target.window, nullptr, nullptr, flags)) return false;
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (!GetProcessTimes(child, &created, &exited, &kernelAfter, &userAfter)) return false;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!K32GetProcessMemoryInfo(child, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory))) return false;
    const auto cpu = cpuTicks(kernelAfter) - cpuTicks(kernelBefore) +
                     cpuTicks(userAfter) - cpuTicks(userBefore);
    std::printf("Benchmark pid=%lu cycles=%d cpu_ms=%.2f elapsed_ms=%.2f private_bytes=%zu\n",
                pid, cycles, cpu / 10000.0, elapsed, memory.PrivateUsage);
    std::fflush(stdout);
    return true;
}
#endif

} // namespace

int wmain(int argc, wchar_t** argv)
{
#if !defined(THEME_HAS_TRACE) && !defined(THEME_BENCHMARK)
    if (argc > 1 && (std::wstring_view(argv[1]) == L"--attach" || std::wstring_view(argv[1]) == L"--attach-enterprise")) {
        try {
            return attachCommand(argc, argv);
        } catch (const std::bad_alloc&) {
            return attachResult(AttachStatus::Error, ERROR_NOT_ENOUGH_MEMORY);
        } catch (const std::exception&) {
            return attachResult(AttachStatus::Error, ERROR_GEN_FAILURE);
        }
    }
#endif
#ifdef THEME_SPREADSHEET_TRACE
    const bool designer = argc == 4 && std::wstring_view(argv[3]) == L"--designer";
    if (designer) --argc;
    if (argc != 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8c.exe or 1cv8.exe> <server\\ref> [--designer]\n", stderr);
        return 2;
    }
#elif defined(THEME_FORM_LINE_TRACE)
    if (argc != 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8c.exe or 1cv8.exe> <server\\ref>\n", stderr);
        return 2;
    }
#elif defined(THEME_SEARCH_RESULTS_TRACE)
    if (argc != 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8.exe> <server\\ref>\n", stderr);
        return 2;
    }
#endif
    const bool enterpriseMode = argc > 3 && std::wstring_view(argv[argc - 1]) == L"--enterprise";
    if (enterpriseMode) --argc;
    const bool profileMode = argc == 5 && std::wstring_view(argv[3]) == L"--palette";
    if (enterpriseMode && !profileMode) { std::fputs("Enterprise requires --palette.\n",stderr); return 2; }
    ThemeProfile profile;
    if (profileMode && !parseProfile(argv[4], profile)) {
        std::fputs("Invalid palette. Expected a zone mask and 48 six-digit RGB colors "
                   "(25, 29, 30, 31, 33, 35, 36, 37, 39 or 44 for legacy palettes).\n", stderr);
        return 2;
    }
    ULONG_PTR selection{};
    for (int i = 3; !profileMode && i < argc; ++i) {
        const std::wstring_view option = argv[i];
        if (option == L"--caption") selection |= enableTreeAndCaption;
        else if (option == L"--bars") selection |= enableTreeAndBars;
        else if (option == L"--properties") selection |= enableTreeAndProperties;
        else if (option == L"--workspace") selection |= enableWorkspace;
        else if (option == L"--status-bar") selection |= enableStatusBar;
        else if (option == L"--window-tabs") selection |= enableWindowTabs;
        else if (option == L"--tree-tabs") selection |= enableTreeTabs;
        else if (option == L"--search-results") selection |= enableSearchResults;
        else {
            std::fwprintf(stderr, L"Unknown option: %ls\n", argv[i]);
            return 2;
        }
    }
    if (argc < 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8.exe> <server\\ref>"
                   " [--caption] [--bars] [--properties] [--workspace] [--status-bar]"
                   " [--window-tabs] [--tree-tabs] [--search-results]\n"
                   "       tree-theme-run.exe <1cv8.exe> <server\\ref> --palette <mask>:<RGB,...>\n"
#if !defined(THEME_HAS_TRACE) && !defined(THEME_BENCHMARK)
                   "       tree-theme-run.exe --attach <pid> <creation-filetime> --palette <mask>:<RGB,...>\n"
#endif
                   , stderr);
        return 2;
    }
    if (!selection) selection = enableTreeOnly;
    const std::wstring exe = std::filesystem::absolute(argv[1]).lexically_normal().wstring();
#ifdef THEME_TOOLTIP_TRACE
    if (!findTooltipTracePlatform(exe)) {
        std::fputs("Unsupported 1cv8.exe hash for tooltip-trace. See RESEARCH.md for supported builds.\n", stderr);
#elif defined(THEME_FORM_LINE_TRACE)
    if (!findFormLineTracePlatform(exe)) {
        std::fputs("Unsupported client hash for form-line-trace. See RESEARCH.md for supported builds.\n", stderr);
#elif defined(THEME_SPREADSHEET_TRACE)
    if (!findOutlineTracePlatform(exe, designer)) {
        std::fputs("Unsupported client hash or mode for spreadsheet tracing. See RESEARCH.md.\n", stderr);
#else
    if (!matchesSha256(exe, exeHash) && !(enterpriseMode && matchesSha256(exe, thinClientHash))) {
        std::fputs("Unsupported 1cv8.exe (expected 8.3.27.2130 x64).\n", stderr);
#endif
        return 2;
    }
    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    const std::wstring dll = (directory / (enterpriseMode ? L"enterprise-theme.dll" : L"tree-theme.dll")).wstring();
    if ((!profileMode || profile.zones) && !std::filesystem::is_regular_file(dll)) {
        std::fputs("tree-theme.dll is missing beside the launcher.\n", stderr);
        return 2;
    }

#if defined(THEME_DOCUMENT_TRACE) || defined(THEME_SEARCH_RESULTS_TRACE)
    ULONG_PTR traceRva{};
    if (themeExportRva(dll, traceExport, traceRva)) {
        std::fprintf(stderr, "The adjacent DLL lacks the %s export.\n", traceExport);
        return 2;
    }
#endif
#ifdef THEME_DOCUMENT_TRACE
    const wchar_t* mode = L" ENTERPRISE ";
#ifdef THEME_SPREADSHEET_TRACE
    if (designer) mode = L" DESIGNER ";
#endif
#else
    const wchar_t* mode = enterpriseMode ? L" ENTERPRISE /iTaxi " : L" DESIGNER ";
#endif
    std::wstring command = quoteCommandArgument(exe) + mode +
        (std::filesystem::path(argv[2]).is_absolute() ? L"/F " : L"/S ") + quoteCommandArgument(argv[2]) + L" /WA+";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "CreateProcessW failed: %lu\n", GetLastError());
        return 1;
    }
    const Handle child(process.hProcess);
    CloseHandle(process.hThread);
    if (profileMode) return applyLaunchProfile(child, process.dwProcessId, dll, profile, enterpriseMode);
#ifdef THEME_HAS_TRACE
#ifdef THEME_SPREADSHEET_TRACE
    std::printf("Started %s PID %lu. Open the spreadsheet first.\n",
                designer ? "Configurator" : "Enterprise", process.dwProcessId);
#elif defined(THEME_FORM_LINE_TRACE)
    std::printf("Started Enterprise PID %lu. Open the target form first.\n", process.dwProcessId);
#elif defined(THEME_SEARCH_RESULTS_TRACE)
    std::printf("Started 1C client PID %lu. Enable the trace before running metadata search.\n",
                process.dwProcessId);
#else
    std::printf("Started 1C client PID %lu.\n", process.dwProcessId);
#endif
    std::puts("Enter e to enable the trace, d to disable it, t to read and reset, or q to quit.");
#else
    std::printf("Started 1C client PID %lu. Open the target tree first.\n", process.dwProcessId);
    std::puts("Enter e to enable, d to disable, q to close this launcher.");
#endif
#ifdef THEME_BENCHMARK
    std::puts("Enter b to measure 300 synchronous redraws of the target window.");
#endif
    std::fflush(stdout);

    ULONG_PTR setter{};
    for (char input[32]; std::fgets(input, sizeof(input), stdin); ) {
        const char action = input[0];
        if (action == 'q') break;
#ifdef THEME_HAS_TRACE
        if (action == 't') {
            const auto error = readTrace(child, process.dwProcessId, dll);
            if (error) std::fprintf(stderr, "Reading trace failed: %lu\n", error);
            if (error == WAIT_TIMEOUT) return 1;
            continue;
        }
#endif
#ifdef THEME_BENCHMARK
        if (action == 'b') {
            if (!benchmark(child, process.dwProcessId))
                std::fputs("Benchmark failed. Use one visible Configurator window.\n", stderr);
            continue;
        }
#endif
        if (action != 'e' && action != 'd') continue;
        if (WaitForSingleObject(child, 0) != WAIT_TIMEOUT) {
            std::fputs("The 1C client has exited.\n", stderr);
            return 1;
        }
        if (!setter && action == 'd') {
            std::puts("Theme is already disabled.");
            std::fflush(stdout);
            continue;
        }
        ThemeProcessLock targetLock;
        DWORD error = lockLaunchedTheme(child, process.dwProcessId, targetLock);
        if (!error && !setter)
            error = loadLaunchedTheme(child, process.dwProcessId, dll, setter, "SetTreeTheme");
        DWORD result{};
        if (!error) {
            const auto setting = action == 'e' ? reinterpret_cast<LPVOID>(selection) : nullptr;
            error = remoteCall(child, setter, setting, result);
        }
        if (error || result) {
            std::fprintf(stderr, "Theme command failed: transport=%lu dll=0x%08lX\n", error, result);
            if (error == WAIT_TIMEOUT) return 1;
            continue;
        }
#ifdef THEME_TOOLTIP_TRACE
        std::puts(action == 'e' ? "Tooltip trace enabled." : "Tooltip trace disabled.");
#elif defined(THEME_FORM_LINE_TRACE)
        std::puts(action == 'e' ? "frmcore line trace enabled." : "frmcore line trace disabled.");
#elif defined(THEME_OUTLINE_TRACE)
        std::puts(action == 'e' ? "Spreadsheet outline trace enabled." : "Spreadsheet outline trace disabled.");
#elif defined(THEME_GRID_TRACE)
        std::puts(action == 'e' ? "Spreadsheet grid trace enabled." : "Spreadsheet grid trace disabled.");
#elif defined(THEME_SEARCH_RESULTS_TRACE)
        std::puts(action == 'e' ? "Search results trace enabled." : "Search results trace disabled.");
#else
        std::puts(action == 'e' ? "Theme enabled. Repaint the tree." :
                                  "Theme disabled. Repaint the tree.");
#endif
        std::fflush(stdout);
    }
    if (setter && WaitForSingleObject(child, 0) == WAIT_TIMEOUT) {
        ThemeProcessLock targetLock;
        DWORD result{};
        DWORD error = lockLaunchedTheme(child, process.dwProcessId, targetLock);
        if (!error) error = remoteCall(child, setter, nullptr, result);
        if (error || result) {
            std::fprintf(stderr, "Disabling theme failed: transport=%lu dll=%lu\n", error, result);
            return 1;
        }
    }
    return 0;
}
