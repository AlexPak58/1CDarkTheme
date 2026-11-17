#include "platform.hpp"
#include <cstdio>
#include <cstdlib>
#include <dwmapi.h>
#include <initializer_list>

namespace {
constexpr COLORREF systemDefault = 0xFFFFFFFF;
struct WindowColors {
    COLORREF background{systemDefault};
    COLORREF text{systemDefault};
};
const HWND oldWindow = reinterpret_cast<HWND>(ULONG_PTR{0x100});
const HWND newWindow = reinterpret_cast<HWND>(ULONG_PTR{0x200});
WindowColors oldColors, newColors;
unsigned writes{};
unsigned failureMask{};

void faults(std::initializer_list<unsigned> steps)
{
    writes = failureMask = 0;
    for (const auto step : steps) failureMask |= 1u << step;
}

DWORD WINAPI fixtureWindowOwner(HWND window, LPDWORD pid)
{
    if (window != oldWindow && window != newWindow) return 0;
    *pid = GetCurrentProcessId();
    return window == oldWindow ? 1u : 2u;
}

HRESULT WINAPI fixtureSetWindowAttribute(HWND window, DWORD attribute, LPCVOID arg, DWORD size)
{
    if (size != sizeof(COLORREF)) return E_INVALIDARG;
    if (failureMask & (1u << ++writes)) return E_ACCESSDENIED;
    auto& colors = (window == oldWindow) ? oldColors : newColors;
    auto& value = attribute == 35 ? colors.background : colors.text;
    value = *static_cast<const COLORREF*>(arg);
    return S_OK;
}
}

#define GetWindowThreadProcessId fixtureWindowOwner
#define DwmSetWindowAttribute fixtureSetWindowAttribute
#include "theme.cpp"
#undef GetWindowThreadProcessId
#undef DwmSetWindowAttribute

void check(bool ok, const char* details)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", details);
        std::exit(1);
    }
}

int main()
{
    const CaptionTarget oldTarget{oldWindow, 1}, newTarget{newWindow, 2};
    constexpr CaptionColors first{0xA0B0C0, 0xB0C0D0};
    constexpr CaptionColors second{0x102030, 0x203040};
    const auto failed = static_cast<DWORD>(E_ACCESSDENIED);
    const auto retained = [&] {
        return appliedCaption.target == oldTarget && oldColors.background == first.background &&
            oldColors.text == first.text && newColors.background == systemDefault &&
            newColors.text == systemDefault;
    };

    faults({1});
    check(transitionCaption(oldTarget, first) == failed, "first background write fails");
    check(!appliedCaption.target.window && oldColors.background == systemDefault,
          "failed first write does not claim the caption");
    faults({});
    check(transitionCaption(oldTarget, first) == ERROR_SUCCESS && retained(), "apply old caption");
    faults({2});
    check(transitionCaption(oldTarget, second) == failed && retained(), "same-window text failure restores both");
    faults({2});
    check(transitionCaption({}, {}) == failed && retained(), "disable failure restores the original caption");
    faults({4});
    check(transitionCaption(newTarget, second) == failed && retained(),
          "switch text failure rolls back against the exact old and new HWND");
    faults({4, 5});
    check(transitionCaption(newTarget, second) == themeStateUncertain && captionStateUncertain,
          "failed rollback reports uncertainty");
    check(appliedCaption.target == oldTarget && oldColors.background == first.background &&
          oldColors.text == first.text, "still attempt old window restoration after new-window rollback fails");
    const unsigned priorWrites = writes;
    check(transitionCaption({}, {}) == themeStateUncertain && writes == priorWrites,
          "latched uncertainty prevents further modifications");
    std::puts("PASS: caption ownership, partial writes, exact rollback and latched uncertainty");
}
