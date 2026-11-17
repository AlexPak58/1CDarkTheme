#pragma once

#include "platform.hpp"
#include <cwchar>

inline constexpr char themeProfileAttachExport[] = "ThemeProfileAttachV6";
inline constexpr DWORD themeStateUncertain = 0x20000001; // Application-defined Win32 code.

struct DesignerWindow {
    HWND window{};
    DWORD thread{};
    bool operator==(const DesignerWindow&) const = default;
};

inline DesignerWindow designerMainWindow(HWND window, DWORD expectedPid)
{
    DWORD pid{};
    const DWORD thread = GetWindowThreadProcessId(window, &pid);
    if (!thread || pid != expectedPid || !IsWindowVisible(window) || !IsWindowEnabled(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return {};
    wchar_t name[32]{};
    const int length = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (length <= 0) return {};
    const std::wstring_view windowClass(name, static_cast<size_t>(length));
    if (windowClass != L"V8TopLevelFrameSDI" && windowClass != L"V8TopLevelFrame") return {};
    return {window, thread};
}

namespace theme_detail {
struct MainWindowSearch {
    DWORD pid;
    DesignerWindow found;
};

inline BOOL CALLBACK findMainWindow(HWND window, LPARAM param)
{
    auto& search = *reinterpret_cast<MainWindowSearch*>(param);
    const auto candidate = designerMainWindow(window, search.pid);
    if (!candidate.window) return TRUE;
    if (search.found.window) {
        search.found = {};
        return FALSE;
    }
    search.found = candidate;
    return TRUE;
}
}

inline DesignerWindow findDesignerMainWindow(DWORD pid)
{
    theme_detail::MainWindowSearch search{pid, {}};
    if (!EnumWindows(theme_detail::findMainWindow, reinterpret_cast<LPARAM>(&search))) return {};
    return search.found;
}

inline bool isDesignerCommandLine(const wchar_t* commandLine)
{
    if (!commandLine || !*commandLine) return false;
    const HMODULE shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!shell) return false;
    using ParseArguments = LPWSTR* (WINAPI*)(LPCWSTR, int*);
    const auto parse = reinterpret_cast<ParseArguments>(
        GetProcAddress(shell, "CommandLineToArgvW"));
    int count{};
    LPWSTR* arguments = parse ? parse(commandLine, &count) : nullptr;
    const bool designer = arguments && count >= 2 && arguments[0][0] &&
        CompareStringOrdinal(arguments[1], -1, L"DESIGNER", -1, TRUE) == CSTR_EQUAL;
    if (arguments) LocalFree(arguments);
    FreeLibrary(shell);
    return designer;
}

inline ULONGLONG fileTimeValue(const FILETIME& time)
{
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

inline DWORD readProcessCreation(HANDLE process, ULONGLONG& creation)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return GetLastError();
    creation = fileTimeValue(created);
    return ERROR_SUCCESS;
}

inline DWORD checkProcessLifetime(HANDLE process, ULONGLONG expected)
{
    const DWORD wait = WaitForSingleObject(process, 0);
    if (wait != WAIT_TIMEOUT) return wait == WAIT_OBJECT_0 ? ERROR_PROCESS_ABORTED : GetLastError();
    ULONGLONG creation{};
    if (const auto error = readProcessCreation(process, creation)) return error;
    return creation == expected ? ERROR_SUCCESS : ERROR_PROCESS_ABORTED;
}

class ThemeProcessLock {
    HANDLE mutex_{};
    bool owned_{};
public:
    ThemeProcessLock() = default;
    ThemeProcessLock(const ThemeProcessLock&) = delete;
    ThemeProcessLock& operator=(const ThemeProcessLock&) = delete;
    ~ThemeProcessLock() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    DWORD acquire(DWORD pid, ULONGLONG creation, DWORD timeout = 1000) {
        if (mutex_) return ERROR_INVALID_FUNCTION;
        wchar_t name[96]{};
        if (swprintf_s(name, std::size(name), L"Local\\1cDarkTheme.Attach.%lu.%016llX", pid, creation) < 0)
            return ERROR_INVALID_PARAMETER;
        mutex_ = CreateMutexW(nullptr, FALSE, name);
        if (!mutex_) return GetLastError();
        const auto wait = WaitForSingleObject(mutex_, timeout);
        if (wait == WAIT_FAILED) return GetLastError();
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return wait;
    }
};
