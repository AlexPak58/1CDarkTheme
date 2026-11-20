#include "enterprise_toggle.hpp"
#include <cstdio>

namespace {
int assertions{}, failures{}, changes{}, clickedCommands{};
int positionChanges{};
HWND watchedSwitch{}, watchedGear{};
bool dark = true;
bool enabled() { return dark; }
void toggle() { dark = !dark; ++changes; }
void check(bool valid, const char* message) {
    ++assertions;
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
LRESULT CALLBACK parentProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) ++clickedCommands;
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK watchPlacement(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (message.message == WM_WINDOWPOSCHANGING &&
            (message.hwnd == watchedSwitch || message.hwnd == watchedGear)) ++positionChanges;
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
HWND parent() {
    // Visible to IsWindowVisible, but never displayed on a monitor, activated,
    // placed on the taskbar or connected to any 1C process.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"EnterpriseToggleUnitParent", L"",
        WS_POPUP | WS_VISIBLE, -30000, -30000, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
void publish(HWND window) {
    enterprise_toggle::detail::Snapshot next{window, {80,3,122,27}, true, false};
    next.settingsRect = {55,3,77,27};
    enterprise_toggle::detail::publish(next);
}
void owned(HWND button, HWND expectedParent) {
    DWORD pid{};
    const DWORD thread = GetWindowThreadProcessId(button, &pid);
    check(button && IsWindow(button) && IsWindowVisible(button), "button is an existing visible child");
    check(GetParent(button) == expectedParent, "button belongs to the current parent");
    check(pid == GetCurrentProcessId() && thread == GetCurrentThreadId(), "button belongs to this test process and thread");
    check(enterprise_toggle::detail::observedButton.load() == button, "observer sees the current child handle");
}
}

int main() {
    WNDCLASSW definition{};
    definition.lpfnWndProc = parentProcedure;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = L"EnterpriseToggleUnitParent";
    if (!RegisterClassW(&definition)) return 2;
    enterprise_toggle::detail::State state{};
    state.enabled = enabled; state.toggle = toggle;
    const HWND first = parent();
    if (!first) { UnregisterClassW(definition.lpszClassName, definition.hInstance); return 2; }

    publish(first);
    check(enterprise_toggle::detail::tick(state), "initial tick continues");
    owned(state.button, first);
    check(dark && changes == 0, "creating the control does not change theme state");
    const HWND firstButton = state.button;
    check(DestroyWindow(first) != FALSE, "synthetic startup parent destroyed");
    check(!IsWindow(firstButton) && !state.button && !state.original, "parent destruction removes child and subclass state");
    // Reuse the last snapshot: this reproduces the interval between the old
    // frame disappearing and the observer publishing the replacement frame.
    check(enterprise_toggle::detail::tick(state), "lost startup parent must not terminate the UI worker");
    check(!state.parent && !state.button && !state.original && !enterprise_toggle::detail::observedButton.load(),
          "lost parent clears stale child and original procedure");
    check(enterprise_toggle::detail::tick(state), "worker survives another tick without a main window");
    check(dark && changes == 0, "startup replacement does not toggle the theme");

    const HWND second = parent();
    if (!second) { UnregisterClassW(definition.lpszClassName, definition.hInstance); return 2; }
    publish(second);
    check(enterprise_toggle::detail::tick(state), "fresh parent tick continues");
    owned(state.button, second);
    watchedSwitch=state.button; watchedGear=state.settings;
    check(watchedGear && IsWindowVisible(watchedGear), "gear is visible beside the switch");
    const auto hook=SetWindowsHookExW(WH_CALLWNDPROC,watchPlacement,nullptr,GetCurrentThreadId());
    check(hook != nullptr,"observe positioning messages in the test window");
    publish(second);
    ValidateRect(watchedSwitch,nullptr);
    ValidateRect(watchedGear,nullptr);
    for (int pass=0; pass<4; ++pass) enterprise_toggle::detail::tick(state);
    check(positionChanges==0,"stable ticks do not reposition or show either control again");
    check(!GetUpdateRect(watchedSwitch,nullptr,FALSE) && !GetUpdateRect(watchedGear,nullptr,FALSE),
          "stable ticks do not schedule idle repaints");
    enterprise_toggle::detail::Snapshot moved{second, {90,3,132,27}, true, false};
    moved.settingsRect = {65,3,87,27};
    enterprise_toggle::detail::publish(moved);
    enterprise_toggle::detail::tick(state);
    check(positionChanges==2,"new geometry moves both controls exactly once");
    enterprise_toggle::detail::tick(state);
    check(positionChanges==2,"another tick leaves the new geometry alone");
    if (hook) UnhookWindowsHookEx(hook);
    SendMessageW(state.button, BM_CLICK, 0, 0);
    check(!dark && changes == 1, "recreated control invokes its retained callback exactly once");
    check(clickedCommands == 0, "activation never forwards BN_CLICKED to the application parent");
    SendMessageW(state.button, BM_CLICK, 0, 0);
    check(dark && changes == 2, "recreated control can restore the enabled state");

    const HWND third = parent();
    if (!third) {
        enterprise_toggle::detail::discardButton(state); DestroyWindow(second);
        UnregisterClassW(definition.lpszClassName, definition.hInstance); return 2;
    }
    publish(third);
    check(enterprise_toggle::detail::tick(state), "switching to a different live parent continues");
    owned(state.button, third);
    check(IsWindow(second) != FALSE, "reparenting destroys only our child, not the previous application window");
    check(dark && changes == 2 && clickedCommands == 0, "reparenting preserves theme state without application commands");

    enterprise_toggle::detail::discardButton(state);
    check(!state.button && !state.parent && !state.original && !enterprise_toggle::detail::observedButton.load(), "explicit teardown clears owned resources");
    check(DestroyWindow(second) && DestroyWindow(third), "all synthetic parent windows cleaned up");
    enterprise_toggle::detail::publish({});
    check(UnregisterClassW(definition.lpszClassName, definition.hInstance) != FALSE, "test window class cleaned up");
    std::printf("%s: %d assertions, %d failures\n", failures ? "FAIL" : "PASS", assertions, failures);
    return failures ? 1 : 0;
}
