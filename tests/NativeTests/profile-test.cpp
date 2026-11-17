#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "platform.hpp"

namespace {
const HWND firstWindow = reinterpret_cast<HWND>(ULONG_PTR{0x100});
const HWND secondWindow = reinterpret_cast<HWND>(ULONG_PTR{0x200});
const wchar_t* fixtureClass = L"#32770";
bool visible = true;
bool enabled = true;
bool wrongPid{};
bool windowOwned{};
bool toolWindow{};
bool lacksCaption{};
bool duplicateMainWindow{};

DWORD WINAPI fixtureWindowOwner(HWND, LPDWORD pid)
{
    *pid = wrongPid ? 0 : GetCurrentProcessId();
    return 1;
}

BOOL WINAPI fixtureVisible(HWND) { return visible; }
BOOL WINAPI fixtureEnabled(HWND) { return enabled; }
HWND WINAPI fixtureGetWindow(HWND, UINT) { return windowOwned ? secondWindow : nullptr; }

LONG_PTR WINAPI fixtureWindowStyle(HWND, int index)
{
    if (index == GWL_EXSTYLE) return toolWindow ? WS_EX_TOOLWINDOW : 0;
    return lacksCaption ? 0 : WS_CAPTION;
}

int WINAPI fixtureGetClass(HWND, LPWSTR name, int count)
{
    if (wcscpy_s(name, static_cast<size_t>(count), fixtureClass)) return 0;
    return static_cast<int>(wcslen(name));
}

BOOL WINAPI fixtureEnumerate(WNDENUMPROC callback, LPARAM param)
{
    if (!callback(firstWindow, param)) return FALSE;
    return !duplicateMainWindow || callback(secondWindow, param);
}
}

#define GetWindowThreadProcessId fixtureWindowOwner
#define IsWindowVisible fixtureVisible
#define IsWindowEnabled fixtureEnabled
#define GetWindow fixtureGetWindow
#define GetWindowLongPtrW fixtureWindowStyle
#define GetClassNameW fixtureGetClass
#define EnumWindows fixtureEnumerate
#include "theme.cpp"
#undef GetWindowThreadProcessId
#undef IsWindowVisible
#undef IsWindowEnabled
#undef GetWindow
#undef GetWindowLongPtrW
#undef GetClassNameW
#undef EnumWindows

void check(bool success, const char* details)
{
    if (!success) {
        std::fprintf(stderr, "FAIL: %s\n", details);
        std::exit(1);
    }
}

int main(int argc, char** argv)
{
    check(argc == 2, "expected process mode");
    installationResult = ERROR_SUCCESS;  // Bypass production code patching in this standalone test.
    ThemeProfile profile;
    check(profile.version == 6 && profile.size == 152 && profile.zones == 16127 &&
          profile.colors[panelHeaderBackground] == 0x1F2938 && profile.colors[panelHeaderText] == 0xE5EBF5,
          "native defaults include panel headers");
    profile.zones = enableSearchResults;
    if (std::strcmp(argv[1], "DESIGNER") != 0) {
        check(SetThemeProfile(&profile) == ERROR_NOT_SUPPORTED, "DLL rejects Enterprise mode");
        check(SetTreeTheme(reinterpret_cast<LPVOID>(enableTreeOnly)) == ERROR_NOT_SUPPORTED,
              "console export rejects Enterprise mode");
        std::puts("PASS: DLL mode guards");
        return 0;
    }

    const DWORD pid = GetCurrentProcessId();
    check(!findDesignerMainWindow(pid).window, "reject native startup dialog");
    installationResult = ERROR_NOT_READY;
    check(SetThemeProfile(&profile) == ERROR_NOT_READY && installationResult == ERROR_NOT_READY &&
          activeProfile.load()->zones == 0, "nonzero profile defers before installation without a main window");
    ThemeProfile disabled = profile;
    disabled.zones = 0;
    check(SetThemeProfile(&disabled) == ERROR_SUCCESS, "disable is allowed before a main window appears");
    installationResult = ERROR_SUCCESS;
    fixtureClass = L"V8TopLevelFrameSDI";
    check(findDesignerMainWindow(pid).window == firstWindow, "accept SDI Configurator main window");
    fixtureClass = L"V8TopLevelFrame";
    check(findDesignerMainWindow(pid).window == firstWindow, "accept classic Configurator main window");
    visible = false;
    check(!findDesignerMainWindow(pid).window, "reject invisible main window");
    visible = true;
    enabled = false;
    check(!findDesignerMainWindow(pid).window, "reject main window blocked by a modal dialog");
    check(SetThemeProfile(&profile) == ERROR_NOT_READY, "nonzero profile defers while main is disabled");
    check(SetThemeProfile(&disabled) == ERROR_SUCCESS, "disable is allowed while main is blocked");
    enabled = true;
    wrongPid = true;
    check(!findDesignerMainWindow(pid).window, "reject a different PID");
    wrongPid = false;
    windowOwned = true;
    check(!findDesignerMainWindow(pid).window, "reject an owned window of the same class");
    windowOwned = false;
    toolWindow = true;
    check(!findDesignerMainWindow(pid).window, "reject tool window");
    toolWindow = false;
    lacksCaption = true;
    check(!findDesignerMainWindow(pid).window, "reject window without caption");
    lacksCaption = false;
    duplicateMainWindow = true;
    check(!findDesignerMainWindow(pid).window, "require a unique main window");
    duplicateMainWindow = false;
    std::puts("PASS: startup dialog rejected, main window required for nonzero profiles, zero allowed");

    profile.colors[searchResultsBackground] = 0x123456;
    check(SetThemeProfile(&profile) == ERROR_SUCCESS, "initial profile");
    const auto first = activeProfile.load(std::memory_order_acquire);
    check(first->zones == enableSearchResults && first->colors[searchResultsBackground] == 0x123456,
          "initial immutable snapshot");
    check(SetThemeProfile(&profile) == ERROR_SUCCESS && activeProfile.load() == first, "deduplicate");
    profile.zones = enablePanelHeaders;
    profile.colors[panelHeaderBackground] = 0x448855;
    profile.colors[panelHeaderText] = 0xCCDDEE;
    check(SetThemeProfile(&profile) == ERROR_SUCCESS &&
          activeProfile.load()->zones == enablePanelHeaders &&
          activeProfile.load()->colors[panelHeaderBackground] == 0x448855 &&
          activeProfile.load()->colors[panelHeaderText] == 0xCCDDEE, "accept panel header palette in isolation");
    profile.zones = enableTreeOnly;
    profile.colors[treeBackground] = 0x345678;
    check(SetThemeProfile(&profile) == ERROR_SUCCESS, "second profile");
    const auto second = activeProfile.load(std::memory_order_acquire);
    check(second != first && second->zones == enableTreeOnly && second->colors[treeBackground] == 0x345678,
          "second snapshot complete");
    check(first->zones == enableSearchResults && first->colors[searchResultsBackground] == 0x123456,
          "original snapshot remains valid for in-flight readers");
    profile.zones = 0;
    fixtureClass = L"#32770";
    check(SetThemeProfile(&profile) == ERROR_SUCCESS && activeProfile.load()->zones == 0,
          "disable after the main window disappears");
    fixtureClass = L"V8TopLevelFrame";
    profile.zones = enableTreeOnly;
    check(SetThemeProfile(&profile) == ERROR_SUCCESS && activeProfile.load() == second,
          "reenable with retained palette");
    check(SetTreeTheme(reinterpret_cast<LPVOID>(enableTreeOnly)) == ERROR_SUCCESS &&
          activeProfile.load()->colors[treeBackground] == defaultProfile.colors[treeBackground],
          "existing console enable still selects default palette");
    check(SetTreeTheme(nullptr) == ERROR_SUCCESS && activeProfile.load()->zones == 0,
          "existing console disable");
    profile.version = themeProfileVersion - 1;
    check(SetThemeProfile(&profile) == ERROR_INVALID_PARAMETER, "reject incompatible profile version");
    check(SetThemeProfile(reinterpret_cast<LPVOID>(8)) == ERROR_INVALID_PARAMETER, "reject unreadable payload");
    std::puts("PASS: profile reapply, coherent retained snapshots, deduplication, disable/reenable, console, ABI and payload checks");
}
