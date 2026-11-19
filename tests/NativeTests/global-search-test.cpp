#include "devtool_binding.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr ULONG_PTR fakeFrame = 0x180000000;
constexpr ULONG_PTR fakeGraphics = 0x200000000;
constexpr ULONG_PTR fakeFrontend = 0x280000000;
constexpr ULONG_PTR fakeDevtool = 0x300000000;
const HWND testWindow = reinterpret_cast<HWND>(ULONG_PTR{0x100});
const PTP_TIMER testTimer = reinterpret_cast<PTP_TIMER>(ULONG_PTR{0x200});
PTP_TIMER_CALLBACK registeredCallback{};
void* callbackContext{};
ULONGLONG now = 100;
bool armed{};
bool mapped{};
bool unexpectedWindow{};
unsigned redraws{};

void check(bool ok, const char* reason)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

PTP_TIMER WINAPI fixtureCreate(PTP_TIMER_CALLBACK callback, void* context, PTP_CALLBACK_ENVIRON)
{
    registeredCallback = callback;
    callbackContext = context;
    return testTimer;
}

VOID WINAPI fixtureTimer(PTP_TIMER timer, PFILETIME due, DWORD period, DWORD)
{
    check(timer == testTimer && !period, "one-shot timer on the optional binding");
    armed = due != nullptr;
}

ULONGLONG WINAPI fixtureTick() { return now; }

DWORD WINAPI fixtureOwner(HWND window, LPDWORD pid)
{
    *pid = unexpectedWindow ? 0 : GetCurrentProcessId();
    return window == testWindow ? 1u : 0u;
}

BOOL WINAPI fixtureTrue(HWND window) { return window == testWindow; }
BOOL WINAPI fixtureFalse(HWND) { return FALSE; }
HWND WINAPI fixtureGetWindow(HWND, UINT) { return nullptr; }

LONG_PTR WINAPI fixtureStyle(HWND, int index) { return index == GWL_STYLE ? WS_CAPTION : 0; }

int WINAPI fixtureClass(HWND, LPWSTR name, int capacity)
{
    constexpr wchar_t value[] = L"V8TopLevelFrameSDI";
    check(wcscpy_s(name, static_cast<size_t>(capacity), value) == 0, "fixture window class fits");
    return static_cast<int>(std::size(value) - 1);
}

BOOL WINAPI fixtureEnum(WNDENUMPROC callback, LPARAM state) { return callback(testWindow, state); }

BOOL WINAPI fixtureRedraw(HWND window, const RECT* region, HRGN clip, UINT flags)
{
    check(window == testWindow && !region && !clip &&
          flags == (RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN), "only request asynchronous redraw");
    ++redraws;
    return TRUE;
}
}

namespace devtool_detail {
Attempt fixtureInspect(HMODULE frame) noexcept
{
    check(reinterpret_cast<ULONG_PTR>(frame) == fakeFrame, "use existing frame path as platform anchor");
    return mapped ? Attempt{Result::Bound, fakeDevtool} : Attempt{};
}
}

#define CreateThreadpoolTimer fixtureCreate
#define SetThreadpoolTimer fixtureTimer
#define GetTickCount64 fixtureTick
#define inspectLoadedModule fixtureInspect
#define GetWindowThreadProcessId fixtureOwner
#define IsWindowVisible fixtureTrue
#define IsWindowEnabled fixtureTrue
#define IsIconic fixtureFalse
#define GetWindow fixtureGetWindow
#define GetWindowLongPtrW fixtureStyle
#define GetClassNameW fixtureClass
#define EnumWindows fixtureEnum
#define RedrawWindow fixtureRedraw
#include "theme.cpp"
#undef RedrawWindow
#undef EnumWindows
#undef GetClassNameW
#undef GetWindowLongPtrW
#undef GetWindow
#undef IsIconic
#undef IsWindowEnabled
#undef IsWindowVisible
#undef GetWindowThreadProcessId
#undef inspectLoadedModule
#undef GetTickCount64
#undef SetThreadpoolTimer
#undef CreateThreadpoolTimer

namespace {
struct Grid {
    std::array<ULONG_PTR, 0x800 / sizeof(ULONG_PTR)> control{};
    std::array<ULONG_PTR, 4> provider{};
    ULONG_PTR site{};
    ULONG_PTR owner{};

    Grid()
    {
        control[0] = fakeFrame + 0xA8A278;
        control[0x7B0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(provider.data());
        control[0x7F0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(provider.data() + 3);
        control[8 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(&site);
        control[0x270 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(&owner);
        provider[0] = fakeDevtool + globalSearchProviderRva;
        provider[3] = fakeDevtool + globalSearchRowsRva;
        site = fakeFrame + globalSearchSiteRva;
    }

    ULONG_PTR address() const { return reinterpret_cast<ULONG_PTR>(control.data()); }

    ListKind kind() const { return classifyListControl(address(), fakeFrame, fakeFrontend, fakeDevtool); }
};

void testMatcher()
{
    Grid grid;
    check(grid.kind() == ListKind::SearchResults, "exact global root/provider/rows/alias/site");
    check(classifyListControl(grid.address(), fakeFrame, fakeFrontend) == ListKind::Unknown,
          "global matcher requires verified devtool base");
    grid.control[0] = fakeFrame + 0xA88AC0;
    check(grid.kind() == ListKind::Unknown, "aggregated root is not global search");
    grid.control[0] = fakeFrame + 0xA8A278;
    grid.provider[3] = fakeFrontend + 0xD61F70;
    check(grid.kind() == ListKind::Unknown, "mixed module table pair rejected");
    grid.provider[3] = fakeDevtool + globalSearchRowsRva;
    grid.control[0x7F0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(&grid.provider[2]);
    grid.provider[2] = grid.provider[3];
    check(grid.kind() == ListKind::Unknown, "right vtables on wrong alias do not match");
    grid.control[0x7F0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(&grid.provider[3]);
    grid.site = fakeFrame + 0xB20908;
    check(grid.kind() == ListKind::Unknown, "unrelated standalone editor grid site rejected");
    grid.site = fakeFrame + 0xA61258;
    check(grid.kind() == ListKind::Dropdown, "dropdown retains its established identity");
    grid.site = fakeFrame + globalSearchSiteRva;
    grid.provider[0] = fakeDevtool + globalSearchProviderRva + 8;
    check(grid.kind() == ListKind::Unknown, "unknown provider does not match");

    grid.provider[0] = fakeFrontend + 0xD62048;
    grid.provider[3] = fakeFrontend + 0xD61F70;
    check(grid.kind() == ListKind::SearchResults, "standalone metadata search retains its pair");
    grid.control[0] = fakeFrame + 0xA88AC0;
    check(grid.kind() == ListKind::SearchResults, "aggregated metadata search retains its pair");
    grid.provider[0] = grid.provider[3] = 0;
    grid.owner = fakeFrontend + 0xD95480;
    check(grid.kind() == ListKind::MetadataTree, "metadata tree keeps its own zone");

    constexpr ULONG_PTR top = (std::numeric_limits<ULONG_PTR>::max)();
    const auto high = top - 7;
    grid.control[0] = fakeFrame + 0xA8A278;
    grid.control[0x7B0 / sizeof(ULONG_PTR)] = high;
    grid.control[0x7F0 / sizeof(ULONG_PTR)] = high + 0x18;
    check(grid.kind() == ListKind::Unknown &&
          classifyListControl(high, fakeFrame, fakeFrontend, fakeDevtool) == ListKind::Unknown,
          "overflowing fields and aliases rejected before dereference");
    auto* noAccess = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    check(noAccess != nullptr, "allocate inaccessible provider");
    grid.control[0x7B0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(noAccess);
    grid.control[0x7F0 / sizeof(ULONG_PTR)] = reinterpret_cast<ULONG_PTR>(noAccess) + 0x18;
    check(grid.kind() == ListKind::Unknown &&
          classifyListControl(reinterpret_cast<ULONG_PTR>(noAccess), fakeFrame, fakeFrontend, fakeDevtool) == ListKind::Unknown,
          "inaccessible root and provider fail closed");
    check(VirtualFree(noAccess, 0, MEM_RELEASE) != FALSE, "release inaccessible fixture");
}

void testGuardedReads()
{
    Grid grid;
    auto* page = static_cast<ULONG_PTR*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate guard fixture");
    DWORD previous{};
    const auto guard = [&] {
        check(VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &previous) != FALSE,
              "guard the populated fixture");
    };
    const auto stillGuarded = [&] {
        MEMORY_BASIC_INFORMATION region{};
        return VirtualQuery(page, &region, sizeof(region)) && (region.Protect & PAGE_GUARD);
    };
    std::memcpy(page, grid.control.data(), sizeof(grid.control));
    const auto root = reinterpret_cast<ULONG_PTR>(page);
    check(classifyListControl(root, fakeFrame, fakeFrontend, fakeDevtool) == ListKind::SearchResults,
          "root fixture contains an exact identity before guarding");
    guard();
    check(classifyListControl(root, fakeFrame, fakeFrontend, fakeDevtool) == ListKind::Unknown && stillGuarded(),
          "skip a guarded root without consuming its guard");
    check(VirtualProtect(page, 4096, PAGE_READWRITE, &previous) != FALSE, "reuse the test-owned page");
    std::memcpy(page, grid.provider.data(), sizeof(grid.provider));
    grid.control[0x7B0 / sizeof(ULONG_PTR)] = root;
    grid.control[0x7F0 / sizeof(ULONG_PTR)] = root + 0x18;
    check(grid.kind() == ListKind::SearchResults, "provider fixture contains exact interfaces before guarding");
    guard();
    check(grid.kind() == ListKind::Unknown && stillGuarded(), "skip guarded provider without consuming its guard");
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release guarded fixture");

    constexpr ULONG_PTR kernelAddress = 0xFFFF800000000000;
    MEMORY_BASIC_INFORMATION region{};
    check(!VirtualQuery(reinterpret_cast<LPCVOID>(kernelAddress), &region, sizeof(region)),
          "unqueryable address fixture triggers VirtualQuery failure");
    constexpr DWORD sentinel = 0xFACADE00;
    SetLastError(sentinel);
    const auto kind = classifyListControl(kernelAddress, fakeFrame, fakeFrontend, fakeDevtool);
    const auto lastError = GetLastError();
    check(kind == ListKind::Unknown && lastError == sentinel, "failed region query preserves caller LastError");
}

void apply(const ThemeProfile& profile)
{
    AcquireSRWLockExclusive(&installationLock);
    const auto result = applyProfile(profile, profile.zones);
    ReleaseSRWLockExclusive(&installationLock);
    check(result == ERROR_SUCCESS, "successful profile applies independently of optional readiness");
}

void tick()
{
    check(registeredCallback != nullptr, "optional callback registered");
    registeredCallback(nullptr, callbackContext, testTimer);
}

void testPaint(SiteIndex site, const Grid& grid, DWORD expectedColor)
{
    std::array<ULONG_PTR, 0xE00 / sizeof(ULONG_PTR)> stack{};
    CONTEXT regs{};
    regs.Rsp = reinterpret_cast<ULONG_PTR>(stack.data());
    regs.Rip = reinterpret_cast<ULONG_PTR>(sites[site].address);
    regs.EFlags = 0x202;
    regs.Rcx = 0x1234;
    regs.Rdi = 0x5678;
    regs.Rax = white;
    regs.Rsi = white;
    CONTEXT expected = regs;
    switch (site) {
    case rowFill:
        stack[0x3D8 / 8] = fakeFrame + sharedListPaintReturn;
        stack[0x3B8 / 8] = grid.address() + 0x100;
        expected.R8 = expectedColor;
        break;
    case emptyFill:
        stack[0x98 / 8] = fakeGraphics + 0xABE0C;
        stack[0x148 / 8] = fakeFrame + 0xA4638;
        stack[0x190 / 8] = grid.address();
        stack[0x198 / 8] = fakeFrame + 0x8119D8;
        expected.Rdx = expectedColor;
        break;
    case labelColor:
        regs.R8 = 0xCE000000;
        stack[0x38 / 8] = fakeGraphics + 0x78F51;
        stack[0x578 / 8] = fakeFrame + 0x85F445;
        stack[0xD78 / 8] = fakeFrame + sharedListPaintReturn;
        stack[0xD58 / 8] = grid.address() + 0x100;
        expected.R8 = expected.Rax = expectedColor;
        break;
    default: std::exit(1);
    }
    expected.Rip += 3;
    EXCEPTION_RECORD record{};
    record.ExceptionAddress = sites[site].address;
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    EXCEPTION_POINTERS event{&record, &regs};
    constexpr DWORD sentinel = 0xFACE1234;
    SetLastError(sentinel);
    const auto result = onBreakpoint(&event);
    const auto lastError = GetLastError();
    check(result == EXCEPTION_CONTINUE_EXECUTION && lastError == sentinel &&
          !std::memcmp(&regs, &expected, sizeof(regs)),
          "production replay applies expected color while preserving flags, LastError and unrelated registers");
}

void testTheme()
{
    frameBase = fakeFrame;
    graphicsBase = fakeGraphics;
    frontendBase = fakeFrontend;
    installationResult = ERROR_SUCCESS;
    for (size_t i = 0; i < std::size(sites); ++i)
        sites[i].address = reinterpret_cast<BYTE*>(ULONG_PTR{0x5000} + 0x10 * i);
    Grid grid;
    ThemeProfile enabled;
    enabled.zones = enableSearchResults;
    enabled.colors[searchResultsBackground] = 0x164283;
    enabled.colors[searchResultsText] = 0xFEDCBA;
    apply(enabled);
    check(armed && !devtoolBinding.base(), "production enables lazy binding after profile publication");
    testPaint(rowFill, grid, white);
    testPaint(emptyFill, grid, white);
    testPaint(labelColor, grid, 0xCE000000);
    tick();
    check(armed && !devtoolBinding.base() && !redraws, "missing devtool leaves colors unchanged");

    Grid metadata;
    metadata.provider[0] = fakeFrontend + 0xD62048;
    metadata.provider[3] = fakeFrontend + 0xD61F70;
    const auto darkBg = toBgr(enabled.colors[searchResultsBackground]);
    const auto darkText = 0xCE000000 | enabled.colors[searchResultsText];
    testPaint(rowFill, metadata, darkBg);
    testPaint(labelColor, metadata, darkText);
    mapped = true;
    now += 1000;
    tick();
    check(devtoolBinding.base() == fakeDevtool && !armed && redraws == 1,
          "binding publishes base once and redraws only the owned window");
    unexpectedWindow = true;
    redrawSearchResults();
    check(redraws == 1, "foreign window is not redrawn");
    unexpectedWindow = false;

    testPaint(rowFill, grid, darkBg);
    testPaint(emptyFill, grid, darkBg);
    testPaint(labelColor, grid, darkText);
    grid.site = fakeFrame + 0xB20908;
    testPaint(rowFill, grid, white);
    testPaint(emptyFill, grid, white);
    testPaint(labelColor, grid, 0xCE000000);
    grid.site = fakeFrame + globalSearchSiteRva;
    ThemeProfile disabled = enabled;
    disabled.zones = 0;
    apply(disabled);
    testPaint(rowFill, grid, white);
    testPaint(emptyFill, grid, white);
    testPaint(labelColor, grid, 0xCE000000);
    apply(enabled);
    testPaint(rowFill, grid, darkBg);
    check(!armed && redraws == 1, "re-enable immediately uses retained verification");
}
}

int main()
{
    testMatcher();
    testGuardedReads();
    testTheme();
    std::puts("PASS: exact global identity, existing controls, production replay, late binding and profile disable");
}
