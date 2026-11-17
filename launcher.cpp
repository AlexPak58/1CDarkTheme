#include "platform.hpp"
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

#include <tlhelp32.h>
#include <charconv>
#include <cstdio>
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
    return GetExitCodeThread(worker, &result) ? ERROR_SUCCESS : GetLastError();
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
constexpr char traceExport[] = "ReadSearchResultsTraceV5";
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

bool serverRef(const std::wstring& target)
{
    const auto slash = target.find(L'\\');
    return slash != std::wstring::npos && slash != 0 && slash + 1 < target.size() &&
           target.find(L'\\', slash + 1) == std::wstring::npos &&
           target.find_first_of(L"\"\r\n/") == std::wstring::npos;
}

bool parseProfile(std::wstring_view input, ThemeProfile& profile)
{
    const auto separator = input.find(L':');
    if (separator == std::wstring_view::npos || separator == 0 || separator > 4)
        return false;
    DWORD zones{};
    for (const wchar_t digit : input.substr(0, separator)) {
        if (digit < L'0' || digit > L'9') return false;
        zones = zones * 10 + (digit - L'0');
    }
    if (zones > themeZoneMask) return false;
    profile.zones = zones;
    input.remove_prefix(separator + 1);
    if ((input.size() + 1) % 7 != 0) return false;
    const size_t colorCount = (input.size() + 1) / 7;
    if (colorCount != paletteColorCount &&
        !(colorCount == toolbarBorder && zones < enableToolbarBorder) &&
        !(colorCount == toolbarBandBackground && zones < enableToolbarBand) &&
        !(colorCount == popupMenuBackground && zones < enablePopupMenu))
        return false;
    for (size_t i = 0; i < colorCount; ++i) {
        char hex[6];
        for (size_t digit = 0; digit < sizeof(hex); ++digit) {
            if (input[digit] > 127) return false;
            hex[digit] = static_cast<char>(input[digit]);
        }
        const auto end = hex + sizeof(hex);
        const auto parsed = std::from_chars(hex, end, profile.colors[i], 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
        if (i + 1 < colorCount) {
            if (input[sizeof(hex)] != L',') return false;
            input.remove_prefix(sizeof(hex) + 1);
        }
    }
    return true;
}

struct ProfileWindow {
    DWORD pid;
    HWND window;
};

BOOL CALLBACK findProfileWindow(HWND window, LPARAM param)
{
    auto& target = *reinterpret_cast<ProfileWindow*>(param);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != target.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;
    target.window = window;
    return FALSE;
}

int applyLaunchProfile(HANDLE child, DWORD pid, const std::wstring& dll, const ThemeProfile& profile)
{
    std::printf("Started Configurator PID %lu.\n", pid);
    std::fflush(stdout);
    if (!profile.zones) return 0;

    ULONG_PTR setter{};
    LPVOID payload{};
    DWORD error = ERROR_TIMEOUT;
    DWORD result{};
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 180000) {
        if (WaitForSingleObject(child, 0) != WAIT_TIMEOUT) {
            error = ERROR_PROCESS_ABORTED;
            break;
        }
        ProfileWindow target{pid, nullptr};
        EnumWindows(findProfileWindow, reinterpret_cast<LPARAM>(&target));
        if (target.window) {
            error = setter ? ERROR_SUCCESS : loadTheme(child, pid, dll, setter, "SetThemeProfile");
            if (error && error != ERROR_MOD_NOT_FOUND) break;
            if (!error && !payload) {
                payload = VirtualAllocEx(child, nullptr, sizeof(profile), MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
                if (!payload || !WriteProcessMemory(child, payload, &profile, sizeof(profile), nullptr)) {
                    error = GetLastError();
                    break;
                }
            }
            if (payload) {
                error = remoteCall(child, setter, payload, result);
                if (error) break;
                if (result != ERROR_MOD_NOT_FOUND && result != static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND))) {
                    if (!result) RedrawWindow(target.window, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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
    const bool profileMode = argc == 5 && std::wstring_view(argv[3]) == L"--palette";
    ThemeProfile profile;
    if (profileMode && !parseProfile(argv[4], profile)) {
        std::fputs("Invalid palette. Expected a zone mask and 31 six-digit RGB colors (25, 29 or 30 for legacy palettes).\n", stderr);
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
        else {
            std::fwprintf(stderr, L"Unknown option: %ls\n", argv[i]);
            return 2;
        }
    }
    if (argc < 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8.exe> <server\\ref>"
                   " [--caption] [--bars] [--properties] [--workspace] [--status-bar]"
                   " [--window-tabs] [--tree-tabs]\n"
                   "       tree-theme-run.exe <1cv8.exe> <server\\ref> --palette <mask>:<RGB,...>\n", stderr);
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
    if (!matchesSha256(exe, exeHash)) {
        std::fputs("Unsupported 1cv8.exe (expected 8.3.27.2325 x64).\n", stderr);
#endif
        return 2;
    }
    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    const std::wstring dll = (directory / L"tree-theme.dll").wstring();
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
    const wchar_t* mode = L" DESIGNER ";
#endif
    std::wstring command = L"\"" + exe + L"\"" + mode + L"/S\"" + argv[2] + L"\" /WA+";
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
    if (profileMode) return applyLaunchProfile(child, process.dwProcessId, dll, profile);
#ifdef THEME_HAS_TRACE
#ifdef THEME_SPREADSHEET_TRACE
    std::printf("Started %s PID %lu. Open the spreadsheet first.\n",
                designer ? "Configurator" : "Enterprise", process.dwProcessId);
#elif defined(THEME_FORM_LINE_TRACE)
    std::printf("Started Enterprise PID %lu. Open the target form first.\n", process.dwProcessId);
#elif defined(THEME_SEARCH_RESULTS_TRACE)
    std::printf("Started Configurator PID %lu. Enable the trace before running metadata search.\n",
                process.dwProcessId);
#else
    std::printf("Started Configurator PID %lu.\n", process.dwProcessId);
#endif
    std::puts("Enter e to enable the trace, d to disable it, t to read and reset, or q to quit.");
#else
    std::printf("Started Configurator PID %lu. Open the target tree first.\n", process.dwProcessId);
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
        DWORD error = setter ? ERROR_SUCCESS :
            loadTheme(child, process.dwProcessId, dll, setter, "SetTreeTheme");
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
        DWORD result{};
        const DWORD error = remoteCall(child, setter, nullptr, result);
        if (error || result) {
            std::fprintf(stderr, "Disabling theme failed: transport=%lu dll=%lu\n", error, result);
            return 1;
        }
    }
    return 0;
}
