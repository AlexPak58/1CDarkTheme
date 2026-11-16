#include "platform.hpp"

#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string_view>

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

DWORD loadTheme(HANDLE child, DWORD pid, const std::wstring& dll, ULONG_PTR& setter)
{
    const auto image = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return GetLastError();
    const auto exportAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(image, "SetTreeTheme"));
    const auto exportRva = exportAddress - reinterpret_cast<ULONG_PTR>(image);
    FreeLibrary(image);
    if (!exportAddress) return ERROR_PROC_NOT_FOUND;

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

bool serverRef(const std::wstring& target)
{
    const auto slash = target.find(L'\\');
    return slash != std::wstring::npos && slash != 0 && slash + 1 < target.size() &&
           target.find(L'\\', slash + 1) == std::wstring::npos &&
           target.find_first_of(L"\"\r\n/") == std::wstring::npos;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    ULONG_PTR selection{};
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view option = argv[i];
        if (option == L"--caption") selection |= enableTreeAndCaption;
        else if (option == L"--bars") selection |= enableTreeAndBars;
        else if (option == L"--properties") selection |= enableTreeAndProperties;
        else if (option == L"--workspace") selection |= enableWorkspace;
        else if (option == L"--status-bar") selection |= enableStatusBar;
        else if (option == L"--window-tabs") selection |= enableWindowTabs;
        else {
            std::fwprintf(stderr, L"Unknown option: %ls\n", argv[i]);
            return 2;
        }
    }
    if (argc < 3 || !serverRef(argv[2])) {
        std::fputs("Usage: tree-theme-run.exe <1cv8.exe> <server\\ref>"
                   " [--caption] [--bars] [--properties] [--workspace] [--status-bar]"
                   " [--window-tabs]\n", stderr);
        return 2;
    }
    if (!selection) selection = enableTreeOnly;
    const std::wstring exe = std::filesystem::absolute(argv[1]).lexically_normal().wstring();
    if (!matchesSha256(exe, exeHash)) {
        std::fputs("Unsupported 1cv8.exe (expected 8.3.27.2325 x64).\n", stderr);
        return 2;
    }
    const auto directory = std::filesystem::path(modulePath(nullptr)).parent_path();
    const std::wstring dll = (directory / L"tree-theme.dll").wstring();
    if (!std::filesystem::is_regular_file(dll)) {
        std::fputs("tree-theme.dll is missing beside the launcher.\n", stderr);
        return 2;
    }

    std::wstring command = L"\"" + exe + L"\" DESIGNER /S\"" + argv[2] + L"\" /WA+";
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
    std::printf("Started Configurator PID %lu. Open the target tree first.\n", process.dwProcessId);
    std::puts("Enter e to enable, d to disable, q to close this launcher.");
    std::fflush(stdout);

    ULONG_PTR setter{};
    for (char input[32]; std::fgets(input, sizeof(input), stdin); ) {
        const char action = input[0];
        if (action == 'q') break;
        if (action != 'e' && action != 'd') continue;
        if (WaitForSingleObject(child, 0) != WAIT_TIMEOUT) {
            std::fputs("Configurator has exited.\n", stderr);
            return 1;
        }
        if (!setter && action == 'd') {
            std::puts("Theme is already disabled.");
            std::fflush(stdout);
            continue;
        }
        DWORD error = setter ? ERROR_SUCCESS : loadTheme(child, process.dwProcessId, dll, setter);
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
        std::puts(action == 'e' ? "Theme enabled. Repaint the tree." :
                                  "Theme disabled. Repaint the tree.");
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
