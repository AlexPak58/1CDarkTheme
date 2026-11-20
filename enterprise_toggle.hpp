#pragma once
#include "platform.hpp"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <shellapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

// The COM MTA observer never owns windows. The independent button thread never
// calls UIA or waits for the observer. Only our child BUTTON is subclassed.
namespace enterprise_toggle {
namespace detail {
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};
struct ControlPixels {
    struct Sample { POINT point{}; COLORREF color{CLR_INVALID}; };
    std::array<Sample,5> samples{};
};
struct State {
    bool (*enabled)(){};
    void (*toggle)(){};
    void (*refreshContent)(HWND){};
    bool (*toggleRetro)(){};
    bool (*isRetro)(){};
    bool (*applyProfile)(const ThemeProfile&){};
    ThemeProfile (*profile)(){};
    HWND (*designerMain)(){};
    HWND parent{}, button{};
    HWND settings{};
    WNDPROC original{};
    WNDPROC settingsOriginal{};
    bool pressed{}, keyboard{}, lastEnabled{}, hover{}, gdiplus{};
    int regionWidth{}, regionHeight{};
    ControlPixels switchPixels{}, gearPixels{};
};
inline std::atomic<bool> started{};
inline std::atomic<bool> stopping{};
inline std::atomic<HWND> observedButton{};
inline std::atomic<HWND> observedSettings{};
inline State state;
struct Snapshot {
    HWND parent{}; RECT rectangle{}; bool visible{}, quit{};
    RECT windowBounds{}; ULONGLONG sampled{};
    RECT settingsRect{};
};
inline SRWLOCK snapshotLock = SRWLOCK_INIT;
inline Snapshot latest;
inline void publish(Snapshot next) {
    next.sampled = GetTickCount64();
    if (next.parent && !GetWindowRect(next.parent, &next.windowBounds)) next.visible = false;
    AcquireSRWLockExclusive(&snapshotLock); latest = next; ReleaseSRWLockExclusive(&snapshotLock);
}
inline Snapshot snapshot() {
    AcquireSRWLockShared(&snapshotLock); const auto value = latest; ReleaseSRWLockShared(&snapshotLock);
    return value;
}
struct Observer { HWND parent{}, button{}, settings{}; IUIAutomation* automation{}; };
inline int scale(UINT dpi, int value) { return MulDiv(value, static_cast<int>(dpi), 96); }
inline UINT dpiFor(HWND window) {
    using Function = UINT(WINAPI*)(HWND);
    static const auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = function ? function(window) : 96;
    return dpi >= 48 && dpi <= 768 ? dpi : 96;
}
struct HeaderColors {
    COLORREF background, icon, accent, knob;
};
inline HeaderColors headerColors(const State& s) {
    if (!s.enabled())
        return {s.designerMain ? RGB(245,242,221) : RGB(250,237,158),
                RGB(56,63,71),RGB(109,120,139),RGB(250,252,255)};
    const auto profile = s.profile ? s.profile() : ThemeProfile{};
    const auto canvas = toBgr(profile.colors[workspaceBackground]);
    const auto bars = toBgr(profile.colors[barsBackground]);
    if (s.designerMain)
        return {bars,toBgr(profile.colors[barsText]),RGB(53,122,235),RGB(250,252,255)};
    if (GetRValue(canvas) >= 128 && GetGValue(canvas) >= 128 && GetBValue(canvas) >= 128)
        return {bars,toBgr(profile.colors[barsText]),toBgr(profile.colors[panelBorder]),RGB(255,248,252)};
    return {RGB(GetRValue(bars)*76/100,GetGValue(bars)*76/100,GetBValue(bars)*76/100),
            toBgr(profile.colors[propertyMutedText]),RGB(53,122,235),RGB(250,252,255)};
}
inline void activate(State& s) {
    if (!s.parent || !IsWindow(s.parent)) return;
    s.toggle();
    if (s.settings) SetWindowTextW(s.settings,L"Настройки темы 1С");
    s.lastEnabled = s.enabled();
    SetWindowTextW(s.button, s.lastEnabled ? L"Тёмный режим: включён" : L"Тёмный режим: выключен");
    InvalidateRect(s.button, nullptr, FALSE);
    RedrawWindow(s.parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (s.refreshContent) s.refreshContent(s.parent);
}
inline void pill(Gdiplus::GraphicsPath& path, float x, float y, float width, float height) {
    path.AddArc(x, y, height, height, 90, 180);
    path.AddArc(x+width-height, y, height, height, 270, 180);
    path.CloseFigure();
}
// Taxi can repaint over the header controls, so periodically repair their
// pixels. Compose each repair off-screen so the background never flashes
// between the track and glyph drawing calls.
struct ControlPaint {
    HWND window;
    ControlPixels& pixels;
    PAINTSTRUCT paint{};
    RECT area{};
    HDC target{}, dc{}, buffer{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};

    ControlPaint(HWND button, ControlPixels& samples) :
        window(button), pixels(samples), target(BeginPaint(window,&paint)), dc(target) {
        GetClientRect(window,&area);
        if (!target || area.right <= 0 || area.bottom <= 0) return;
        buffer = CreateCompatibleDC(target);
        if (!buffer) return;
        bitmap = CreateCompatibleBitmap(target,area.right,area.bottom);
        if (bitmap) previous = SelectObject(buffer,bitmap);
        if (previous && previous != HGDI_ERROR) dc = buffer;
        const auto font = GetCurrentObject(target,OBJ_FONT);
        if (dc == buffer && font) SelectObject(buffer,font);
    }

    ControlPaint(const ControlPaint&) = delete;
    ControlPaint& operator=(const ControlPaint&) = delete;

    ~ControlPaint() {
        if (dc == buffer && buffer) {
            const auto x=area.right/2, y=area.bottom/2;
            const POINT points[]={{x,y},{x-3,y},{x+3,y},{x,y-3},{x,y+3}};
            for (size_t i=0;i<pixels.samples.size();++i)
                pixels.samples[i]={points[i],GetPixel(buffer,points[i].x,points[i].y)};
            BitBlt(target,0,0,area.right,area.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,previous);
        } else pixels={};
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(window,&paint);
    }
};
inline void paint(HWND button, State& s) {
    const ControlPaint frame(button,s.switchPixels);
    const HDC dc = frame.dc;
    RECT area = frame.area;
    if (s.isRetro && s.isRetro()) {
        const auto shade = CreateSolidBrush(RGB(0,0,128));
        FillRect(dc,&area,shade);
        DeleteObject(shade);
        RECT edge=area;
        DrawEdge(dc,&edge,s.pressed ? EDGE_SUNKEN : EDGE_RAISED,BF_RECT|BF_ADJUST);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(255,255,255));
        DrawTextW(dc,L"’98",-1,&edge,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if (GetFocus()==button) DrawFocusRect(dc,&edge);
        return;
    }
    const UINT dpi = dpiFor(button);
    const bool dark = s.enabled(), focused = GetFocus() == button;
    const auto colors = headerColors(s);
    const auto tintTrack = [&] {
        const int mix = s.pressed ? 13 : s.hover ? 18 : 0;
        const int target = s.pressed ? 0 : 255;
        const auto tint = [mix,target](BYTE channel) { return (channel*(100-mix)+target*mix)/100; };
        return RGB(tint(GetRValue(colors.accent)),tint(GetGValue(colors.accent)),tint(GetBValue(colors.accent)));
    };
    const COLORREF track = dark ? tintTrack() :
        (s.pressed ? RGB(132, 144, 160) : s.hover ? RGB(157, 169, 186) : RGB(178, 188, 202));
    const auto backgroundBrush = CreateSolidBrush(colors.background);
    FillRect(dc, &area, backgroundBrush); DeleteObject(backgroundBrush);
    bool painted{};
    if (s.gdiplus) {
        using namespace Gdiplus;
        Graphics graphics(dc);
        if (graphics.GetLastStatus() == Ok) {
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            const float factor = static_cast<float>(dpi)/96.0f;
            graphics.ScaleTransform(factor, factor);
            const float width = static_cast<float>(area.right-area.left)/factor;
            const float height = static_cast<float>(area.bottom-area.top)/factor;
            const auto color = [](COLORREF value) { return Color(255, GetRValue(value), GetGValue(value), GetBValue(value)); };
            GraphicsPath outline; pill(outline, 1.5f, 2.0f, width-3.0f, height-4.0f);
            SolidBrush trackBrush(color(track)); graphics.FillPath(&trackBrush, &outline);
            if (focused) {
                GraphicsPath focus; pill(focus, .5f, .5f, width-1.0f, height-1.0f);
                Pen ring(color(dark ? colors.icon : RGB(40,103,204)), 1.0f);
                graphics.DrawPath(&ring, &focus);
            }
            const float cx = dark ? width-12.0f : 12.0f, cy = height/2.0f;
            const float radius = s.pressed ? 7.5f : 8.0f;
            SolidBrush shadow(Color(45, 0, 0, 0));
            graphics.FillEllipse(&shadow, cx-radius, cy-radius+1.0f, radius*2.0f, radius*2.0f);
            SolidBrush knob(color(colors.knob));
            graphics.FillEllipse(&knob, cx-radius, cy-radius, radius*2.0f, radius*2.0f);
            const Color glyph = color(colors.accent);
            SolidBrush glyphBrush(glyph);
            if (dark) {
                graphics.FillEllipse(&glyphBrush, cx-4.0f, cy-4.5f, 8.0f, 9.0f);
                graphics.FillEllipse(&knob, cx-1.0f, cy-5.5f, 7.0f, 8.0f);
            } else {
                graphics.FillEllipse(&glyphBrush, cx-2.3f, cy-2.3f, 4.6f, 4.6f);
                Pen rays(glyph, 1.1f); rays.SetStartCap(LineCapRound); rays.SetEndCap(LineCapRound);
                for (int index = 0; index < 8; ++index) {
                    const float angle = static_cast<float>(index)*3.14159265f/4.0f;
                    const float dx = std::cos(angle), dy = std::sin(angle);
                    graphics.DrawLine(&rays, cx+dx*3.7f, cy+dy*3.7f, cx+dx*4.7f, cy+dy*4.7f);
                }
            }
            painted = graphics.GetLastStatus() == Ok;
        }
    }
    if (!painted) {
        // GDI+ may be unavailable. Keep a usable switch with basic GDI.
        const auto brush = CreateSolidBrush(track), knob = CreateSolidBrush(colors.knob);
        const auto pen = CreatePen(PS_SOLID, 1, focused ? RGB(80, 150, 250) : track);
        const auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
        RoundRect(dc, scale(dpi, 1), scale(dpi, 2), area.right-scale(dpi, 1), area.bottom-scale(dpi, 2), scale(dpi, 20), scale(dpi, 20));
        SelectObject(dc, knob);
        const int cx = dark ? area.right-scale(dpi, 12) : scale(dpi, 12), cy = area.bottom/2, radius = scale(dpi, 8);
        Ellipse(dc, cx-radius, cy-radius, cx+radius, cy+radius);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(pen); DeleteObject(brush); DeleteObject(knob);
        if (focused) { InflateRect(&area, -1, -1); DrawFocusRect(dc, &area); }
    }
}
inline LRESULT CALLBACK buttonProcedure(HWND button, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(button, GWLP_USERDATA));
    if (!s || !s->original) return DefWindowProcW(button, message, wparam, lparam);
    switch (message) {
    case WM_PAINT: paint(button, *s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (!s->hover) {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, button, 0};
            if (TrackMouseEvent(&tracking)) s->hover = true;
            InvalidateRect(button, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE: s->hover = false; InvalidateRect(button, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        SetFocus(button); SetCapture(button); s->pressed = true; InvalidateRect(button, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        const bool pressed = s->pressed; s->pressed = false;
        if (GetCapture() == button) ReleaseCapture();
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        RECT rect{}; GetClientRect(button, &rect);
        if (pressed && PtInRect(&rect, point)) activate(*s);
        InvalidateRect(button, nullptr, FALSE); return 0;
    }
    case WM_CAPTURECHANGED: s->pressed = false; InvalidateRect(button, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_SPACE || wparam == VK_RETURN) {
            s->keyboard = true; s->pressed = true; InvalidateRect(button, nullptr, FALSE); return 0;
        }
        break;
    case WM_KEYUP:
        if (wparam == VK_SPACE || wparam == VK_RETURN) {
            const bool wasPressed = s->keyboard; s->keyboard = s->pressed = false;
            if (wasPressed) activate(*s);
            InvalidateRect(button, nullptr, FALSE); return 0;
        }
        break;
    case WM_CHAR: if (wparam == L' ' || wparam == L'\r') return 0; break;
    case WM_GETDLGCODE: return DLGC_BUTTON | DLGC_WANTCHARS;
    case WM_SETFOCUS: InvalidateRect(button, nullptr, FALSE); break;
    case WM_KILLFOCUS: s->keyboard = s->pressed = false; InvalidateRect(button, nullptr, FALSE); break;
    case BM_CLICK: activate(*s); return 0; // Accessibility invoke, never BN_CLICKED.
    case WM_NCDESTROY: {
        const auto previous = s->original;
        observedButton.store(nullptr, std::memory_order_release);
        s->button = nullptr; s->original = nullptr;
        return CallWindowProcW(previous, button, message, wparam, lparam);
    }
    }
    return CallWindowProcW(s->original, button, message, wparam, lparam);
}
struct MainWindow { HWND window{}; LONGLONG area{}; };
inline BOOL CALLBACK findMain(HWND window, LPARAM parameter) {
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    RECT rect{}; GetClientRect(window, &rect);
    if (rect.right < 300 || rect.bottom < 180) return TRUE;
    auto& found = *reinterpret_cast<MainWindow*>(parameter);
    const LONGLONG area = static_cast<LONGLONG>(rect.right) * rect.bottom;
    if (area > found.area) { found.window = window; found.area = area; }
    return TRUE;
}
inline bool finiteRect(const RECT& r) {
    return r.left > -1000000 && r.top > -1000000 && r.right < 1000000 && r.bottom < 1000000 && r.right > r.left && r.bottom > r.top;
}
inline bool containsRect(const RECT& outer, const RECT& inner) {
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right && outer.bottom >= inner.bottom;
}
inline bool inertContainer(int type) {
    return type == UIA_WindowControlTypeId || type == UIA_PaneControlTypeId || type == UIA_GroupControlTypeId ||
        type == UIA_ToolBarControlTypeId || type == UIA_TitleBarControlTypeId;
}
inline bool interactiveRole(int type) {
    switch (type) {
    case UIA_ButtonControlTypeId: case UIA_EditControlTypeId: case UIA_ComboBoxControlTypeId:
    case UIA_HyperlinkControlTypeId: case UIA_MenuItemControlTypeId: case UIA_CheckBoxControlTypeId:
    case UIA_RadioButtonControlTypeId: case UIA_SliderControlTypeId: case UIA_SpinnerControlTypeId:
    case UIA_TabItemControlTypeId: case UIA_SplitButtonControlTypeId: case UIA_ScrollBarControlTypeId:
    case UIA_ThumbControlTypeId: case UIA_ListItemControlTypeId: case UIA_TreeItemControlTypeId:
    case UIA_DataItemControlTypeId: case UIA_HeaderItemControlTypeId: case UIA_CalendarControlTypeId:
        return true;
    default: return false;
    }
}
enum class Placement { unavailable, blocked, ready };
inline Placement designerPlacement(const Observer& s, RECT& output, RECT& settingsRect) {
    RECT client{};
    POINT origin{};
    if (!GetClientRect(s.parent,&client) || !ClientToScreen(s.parent,&origin)) return Placement::unavailable;
    const auto dpi=dpiFor(s.parent);
    const int gap=scale(dpi,4), width=scale(dpi,42), gearWidth=scale(dpi,23);
    if (client.right<scale(dpi,320)) return Placement::blocked;
    Com<IUIAutomationElement> root;
    Com<IUIAutomationCondition> toolbars;
    Com<IUIAutomationElementArray> rows;
    VARIANT role{};
    role.vt=VT_I4; role.lVal=UIA_ToolBarControlTypeId;
    if (FAILED(s.automation->ElementFromHandle(s.parent,root.put())) || !root.value ||
        FAILED(s.automation->CreatePropertyCondition(UIA_ControlTypePropertyId,role,toolbars.put())) || !toolbars.value ||
        FAILED(root->FindAll(TreeScope_Descendants,toolbars.value,rows.put())) || !rows.value)
        return Placement::unavailable;
    Com<IUIAutomationCondition> all;
    if (FAILED(s.automation->CreateTrueCondition(all.put())) || !all.value) return Placement::unavailable;
    int count{};
    if (FAILED(rows->get_Length(&count)) || count<0 || count>512) return Placement::unavailable;
    RECT menu{};
    int occupiedRight=origin.x;
    for (int i=0;i<count;++i) {
        Com<IUIAutomationElement> bar;
        RECT bounds{};
        if (FAILED(rows->GetElement(i,bar.put())) || !bar.value ||
            FAILED(bar->get_CurrentBoundingRectangle(&bounds))) return Placement::unavailable;
        if (!finiteRect(bounds) || bounds.top<origin.y || bounds.top>origin.y+scale(dpi,5) ||
            bounds.bottom-bounds.top<scale(dpi,16) || bounds.bottom>origin.y+scale(dpi,32)) continue;
        BOOL offscreen=TRUE;
        if (FAILED(bar->get_CurrentIsOffscreen(&offscreen))) return Placement::unavailable;
        if (offscreen) continue;
        if (!finiteRect(menu) || bounds.left<menu.left) menu=bounds;
        Com<IUIAutomationElementArray> items;
        int itemCount{};
        if (FAILED(bar->FindAll(TreeScope_Children,all.value,items.put())) || !items.value ||
            FAILED(items->get_Length(&itemCount)) || itemCount<0 || itemCount>512) return Placement::unavailable;
        for (int n=0;n<itemCount;++n) {
            Com<IUIAutomationElement> item;
            RECT rect{};
            if (FAILED(items->GetElement(n,item.put())) || !item.value ||
                FAILED(item->get_CurrentBoundingRectangle(&rect))) return Placement::unavailable;
            if (finiteRect(rect) && rect.right>occupiedRight) occupiedRight=rect.right;
        }
    }
    if (!finiteRect(menu)) return Placement::blocked;
    const int right=client.right-2*gap, left=right-width, gearLeft=left-gap-gearWidth;
    if (gearLeft+origin.x<occupiedRight+2*gap) return Placement::blocked;
    const LONG top=menu.top-origin.y, bottom=menu.bottom-origin.y;
    output={left,top,right,bottom};
    settingsRect={gearLeft,top,left-gap,bottom};
    return Placement::ready;
}
inline Placement placement(Observer& s, RECT& output, RECT& settingsRect) {
    RECT client{}; if (!GetClientRect(s.parent, &client)) return Placement::unavailable;
    POINT origin{}; if (!ClientToScreen(s.parent, &origin)) return Placement::unavailable;
    const UINT dpi = dpiFor(s.parent);
    Com<IUIAutomationElement> root;
    if (FAILED(s.automation->ElementFromHandle(s.parent, root.put())) || !root.value) return Placement::unavailable;
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_EditControlTypeId;
    Com<IUIAutomationCondition> condition;
    if (FAILED(s.automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.put()))) return Placement::unavailable;
    Com<IUIAutomationElementArray> edits;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.value, edits.put())) || !edits.value) return Placement::unavailable;
    int count{}; if (FAILED(edits->get_Length(&count)) || count < 0 || count > 4096) return Placement::unavailable;
    RECT search{};
    Com<IUIAutomationElement> searchEdit;
    for (int i = 0; i < count; ++i) {
        Com<IUIAutomationElement> edit;
        if (FAILED(edits->GetElement(i, edit.put())) || !edit.value) return Placement::unavailable;
        RECT r{}; BOOL offscreen = TRUE; int pid{};
        if (FAILED(edit->get_CurrentBoundingRectangle(&r)) || !finiteRect(r) ||
            FAILED(edit->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
            FAILED(edit->get_CurrentProcessId(&pid)) || pid != static_cast<int>(GetCurrentProcessId())) continue;
        if (r.top < origin.y || r.top > origin.y + scale(dpi, 32) ||
            r.bottom > origin.y + scale(dpi, 48) || r.right-r.left < scale(dpi, 80)) continue;
        search = r; searchEdit.value = edit.value; searchEdit.value->AddRef(); break;
    }
    if (!searchEdit.value) return Placement::blocked;
    // The Edit rectangle omits the search magnifier. Prefer its nearest small
    // header ancestor extending left of the text; otherwise reserve 24 DPI px.
    int searchLeft = search.left - scale(dpi, 24);
    Com<IUIAutomationTreeWalker> walker;
    Com<IUIAutomationElement> header;
    if (SUCCEEDED(s.automation->get_ControlViewWalker(walker.put())) && walker.value) {
        Com<IUIAutomationElement> current;
        current.value = searchEdit.value; current.value->AddRef();
        bool searchContainerFound{};
        for (int depth = 0; depth < 4; ++depth) {
            Com<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.value, parent.put())) || !parent.value) break;
            RECT r{}; int pid{};
            if (SUCCEEDED(parent->get_CurrentBoundingRectangle(&r)) && finiteRect(r) &&
                SUCCEEDED(parent->get_CurrentProcessId(&pid)) && pid == static_cast<int>(GetCurrentProcessId()) &&
                containsRect(r, search) && r.top >= origin.y && r.bottom <= origin.y+scale(dpi, 48)) {
                if (!searchContainerFound && r.left <= search.left-scale(dpi,16) &&
                    r.right-r.left <= search.right-search.left+scale(dpi,96)) {
                    searchLeft = r.left;
                    searchContainerFound = true;
                }
                if (r.left <= origin.x+scale(dpi,10) && r.right >= origin.x+client.right-scale(dpi,10)) {
                    header.value = parent.value;
                    header.value->AddRef();
                    break;
                }
            }
            current.value->Release(); current.value = parent.value; parent.value = nullptr;
        }
    }
    const int width = scale(dpi, 42), height = scale(dpi, 24), gap = scale(dpi, 4);
    output = {searchLeft-origin.x-gap-width, scale(dpi, 3), searchLeft-origin.x-gap, scale(dpi, 3)+height};
    if (output.left < scale(dpi, 80) || output.right > client.right || output.bottom > client.bottom || output.top < 0)
        return Placement::blocked;
    int gearLeft = output.left - scale(dpi, 26);
    if (gearLeft < scale(dpi,76) && output.right-scale(dpi,48) >= scale(dpi,76)) {
        output.left = output.right-scale(dpi,32);
        gearLeft = output.left-scale(dpi,16);
    }
    RECT screen{origin.x+output.left, origin.y+output.top, origin.x+output.right, origin.y+output.bottom};
    RECT gearScreen{origin.x+gearLeft,screen.top,screen.left-scale(dpi,3),screen.bottom};
    bool gearFits = gearLeft >= scale(dpi, 76);
    // Inspect only this application's UIA subtree. Global point hit testing can
    // return unrelated transparent accessibility overlays above the application.
    Com<IUIAutomationCondition> all;
    if (FAILED(s.automation->CreateTrueCondition(all.put())) || !all.value) return Placement::unavailable;
    Com<IUIAutomationElementArray> descendants;
    auto* collisionRoot = header.value ? header.value : root.value;
    if (FAILED(collisionRoot->FindAll(TreeScope_Descendants, all.value, descendants.put())) || !descendants.value ||
        FAILED(descendants->get_Length(&count)) || count < 0 || count > 4096) return Placement::unavailable;
    for (int i = 0; i < count; ++i) {
        Com<IUIAutomationElement> element;
        if (FAILED(descendants->GetElement(i, element.put())) || !element.value) return Placement::unavailable;
        UIA_HWND native{};
        if (FAILED(element->get_CurrentNativeWindowHandle(&native))) return Placement::unavailable;
        if ((s.button && reinterpret_cast<HWND>(native) == s.button) ||
            (s.settings && reinterpret_cast<HWND>(native) == s.settings)) continue;
        RECT r{}, overlap{};
        if (FAILED(element->get_CurrentBoundingRectangle(&r))) return Placement::unavailable;
        if (!finiteRect(r)) continue;
        const bool hitsSwitch = IntersectRect(&overlap,&screen,&r) != FALSE;
        const bool hitsGear = gearFits && IntersectRect(&overlap,&gearScreen,&r);
        if (!hitsSwitch && !hitsGear) continue;
        BOOL offscreen=TRUE;
        if (FAILED(element->get_CurrentIsOffscreen(&offscreen))) return Placement::unavailable;
        if (offscreen) continue;
        int type{};
        if (FAILED(element->get_CurrentControlType(&type))) return Placement::unavailable;
        if (inertContainer(type)) continue;
        BOOL focusable=TRUE;
        if (!interactiveRole(type) && FAILED(element->get_CurrentIsKeyboardFocusable(&focusable)))
            return Placement::unavailable;
        if (interactiveRole(type) || focusable) {
            if (hitsSwitch) return Placement::blocked;
            gearFits = false;
        }
    }
    if (gearFits) settingsRect = {gearLeft, output.top, output.left-scale(dpi,3), output.bottom};
    return Placement::ready;
}
inline constexpr ULONG_PTR themeTargetMessage = 0x31435447;
inline constexpr ULONG_PTR themeProfileMessage = 0x31435046;

inline std::wstring settingsExecutable() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&settingsExecutable), &module)) return {};
    const auto dll = modulePath(module);
    const auto slash = dll.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : dll.substr(0,slash+1) + L"ConfiguratorThemeSettings.exe";
}
inline DWORD settingsProcess(HWND settings, const std::wstring& executable) {
    if (!settings || executable.empty()) return 0;
    DWORD pid{};
    GetWindowThreadProcessId(settings,&pid);
    if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)) {
        wchar_t path[32768]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        const bool match = QueryFullProcessImageNameW(process,0,path,&length) && _wcsicmp(path,executable.c_str()) == 0;
        CloseHandle(process);
        if (match) return pid;
    }
    return 0;
}
inline void openSettings(State& s) {
    const auto executable = settingsExecutable();
    if (executable.empty()) return;
    if (const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\1CDarkTheme.ShowSettings")) {
        bool connected{};
        const auto settings = FindWindowW(nullptr,L"Тема 1С");
        if (const auto pid = settingsProcess(settings,executable)) {
            AllowSetForegroundWindow(pid);
            struct DesignerTarget { DWORD pid; ThemeProfile profile; };
            const DWORD clientPid=GetCurrentProcessId();
            DesignerTarget target{clientPid,s.designerMain && s.profile ? s.profile() : ThemeProfile{}};
            if (s.designerMain && !s.enabled()) target.profile.zones=0;
            COPYDATASTRUCT data{themeTargetMessage,static_cast<DWORD>(s.designerMain ? sizeof(target) : sizeof(clientPid)),&target};
            DWORD_PTR reply{};
            connected=SendMessageTimeoutW(settings, WM_COPYDATA, reinterpret_cast<WPARAM>(s.settings),
                reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG|SMTO_BLOCK, 750, &reply) && reply==1;
        }
        const bool signaled = (!s.designerMain || connected) && SetEvent(event);
        CloseHandle(event);
        if (signaled || s.designerMain) return;
    }
    const auto argument = std::wstring(s.designerMain ? L"--designer-theme-target " : L"--theme-target ") +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(s.settings));
    ShellExecuteW(s.parent,L"open",executable.c_str(),argument.c_str(),nullptr,SW_SHOWNORMAL);
}
inline void settingsAction(State& s, bool retroGesture = false) {
    if (s.toggleRetro && retroGesture) {
        const bool enabled = s.toggleRetro();
        SetWindowTextW(s.settings, enabled ? L"Настройки темы 1С — режим 1998" : L"Настройки темы 1С");
        RedrawWindow(s.parent,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        if (s.refreshContent) s.refreshContent(s.parent);
        return;
    }
    openSettings(s);
}
inline LRESULT CALLBACK settingsProcedure(HWND button, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(button,GWLP_USERDATA));
    if (!s || !s->settingsOriginal) return DefWindowProcW(button,message,wparam,lparam);
    switch (message) {
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (!data || data->dwData != themeProfileMessage || data->cbData != sizeof(ThemeProfile) ||
            !data->lpData || !s->applyProfile ||
            !settingsProcess(reinterpret_cast<HWND>(wparam),settingsExecutable())) return FALSE;
        ThemeProfile profile;
        std::memcpy(&profile,data->lpData,sizeof profile);
        if (!s->applyProfile(profile)) return FALSE;
        SetWindowTextW(button,L"Настройки темы 1С");
        RedrawWindow(s->parent,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        if (s->refreshContent) s->refreshContent(s->parent);
        return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        const ControlPaint frame(button,s->gearPixels);
        const auto dc = frame.dc;
        RECT area = frame.area;
        const bool retro = s->isRetro && s->isRetro();
        const auto colors = headerColors(*s);
        const auto background = CreateSolidBrush(retro ? RGB(212,208,200) :
            colors.background);
        FillRect(dc,&area,background); DeleteObject(background);
        if (retro) DrawEdge(dc,&area,EDGE_RAISED,BF_RECT|BF_ADJUST);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,retro ? RGB(20,27,44) : colors.icon);
        const auto font = CreateFontW(-(std::min)(scale(dpiFor(button),16),static_cast<int>(area.right)-1),
                                      0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                                      DEFAULT_CHARSET,0,0,0,0,L"Segoe UI Symbol");
        const auto oldFont = font ? SelectObject(dc,font) : nullptr;
        DrawTextW(dc,L"⚙",-1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if (oldFont) SelectObject(dc,oldFont);
        if (font) DeleteObject(font);
        if (GetFocus() == button) DrawFocusRect(dc,&area);
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(button); SetCapture(button); return 0;
    case WM_LBUTTONUP: {
        const bool captured = GetCapture() == button;
        if (captured) ReleaseCapture();
        POINT point{static_cast<short>(LOWORD(lparam)),static_cast<short>(HIWORD(lparam))};
        RECT area{}; GetClientRect(button,&area);
        if (captured && PtInRect(&area,point))
            settingsAction(*s,(wparam & (MK_CONTROL|MK_SHIFT)) == (MK_CONTROL|MK_SHIFT));
        return 0;
    }
    case BM_CLICK: settingsAction(*s); return 0;
    case WM_KEYDOWN: if (wparam == VK_RETURN || wparam == VK_SPACE) return 0; break;
    case WM_KEYUP:
        if (wparam == VK_RETURN || wparam == VK_SPACE) {
            settingsAction(*s,GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        break;
    case WM_CHAR: if (wparam == L' ' || wparam == L'\r') return 0; break;
    case WM_GETDLGCODE: return DLGC_BUTTON|DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(button,nullptr,FALSE); break;
    case WM_NCDESTROY: {
        const auto proc = s->settingsOriginal;
        s->settings = nullptr; s->settingsOriginal = nullptr;
        observedSettings.store(nullptr,std::memory_order_release);
        return CallWindowProcW(proc,button,message,wparam,lparam);
    }
    }
    return CallWindowProcW(s->settingsOriginal,button,message,wparam,lparam);
}
inline void discardButton(State& s) {
    // A startup/login frame can disappear before the final main frame exists.
    // Destroy only a window still owned by this worker, never a recycled HWND.
    observedButton.store(nullptr, std::memory_order_release);
    observedSettings.store(nullptr, std::memory_order_release);
    if (s.settings && GetWindowThreadProcessId(s.settings,nullptr) == GetCurrentThreadId())
        DestroyWindow(s.settings);
    s.settings = nullptr; s.settingsOriginal = nullptr;
    if (s.button) {
        DWORD pid{};
        const DWORD thread = GetWindowThreadProcessId(s.button, &pid);
        if (thread == GetCurrentThreadId() && pid == GetCurrentProcessId()) DestroyWindow(s.button);
    }
    s.button = nullptr; s.original = nullptr; s.parent = nullptr;
    s.regionWidth = s.regionHeight = 0;
    s.pressed = false; s.keyboard = false;
}
inline void placeControl(HWND control, HWND parent, const RECT& desired) {
    RECT existing{};
    const bool visible = IsWindowVisible(control) != FALSE;
    if (GetWindowRect(control,&existing)) {
        MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&existing),2);
        if (visible && EqualRect(&existing,&desired)) return;
    }
    SetWindowPos(control,HWND_TOP,desired.left,desired.top,desired.right-desired.left,
                 desired.bottom-desired.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
inline void repairIfCovered(HWND control, const ControlPixels& pixels) {
    if (!control || !IsWindowVisible(control) || GetUpdateRect(control,nullptr,FALSE)) return;
    const auto dc=GetDC(control);
    if (!dc) return;
    bool covered{};
    for (const auto& sample : pixels.samples) {
        if (sample.color==CLR_INVALID) continue;
        const auto current=GetPixel(dc,sample.point.x,sample.point.y);
        if (current!=CLR_INVALID && current!=sample.color) { covered=true; break; }
    }
    ReleaseDC(control,dc);
    if (covered) InvalidateRect(control,nullptr,FALSE);
}
inline bool tick(State& s) {
    if (s.parent && !IsWindow(s.parent)) discardButton(s);
    const auto current = snapshot();
    if (current.quit || stopping.load(std::memory_order_acquire)) return false;
    if (!current.parent || !current.visible || !IsWindow(current.parent) ||
        !IsWindowVisible(current.parent) || IsIconic(current.parent)) {
        if (s.button) ShowWindow(s.button, SW_HIDE);
        if (s.settings) ShowWindow(s.settings, SW_HIDE);
        return true;
    }
    RECT bounds{};
    if (GetTickCount64() - current.sampled > 5000 || !GetWindowRect(current.parent, &bounds) ||
        !EqualRect(&bounds, &current.windowBounds)) {
        // Never retain stale geometry while a UIA provider is blocked or the
        // application is moving/resizing. A fresh observation will show it.
        if (s.button) ShowWindow(s.button, SW_HIDE);
        if (s.settings) ShowWindow(s.settings, SW_HIDE);
        return true;
    }
    if (s.parent && s.parent != current.parent) discardButton(s);
    s.parent = current.parent;
    const RECT rect = current.rectangle;
    if (!s.button) {
        s.button = CreateWindowExW(0, L"BUTTON", L"Тёмный режим", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                  rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
                                  s.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s.button) return true;
        SetWindowLongPtrW(s.button, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        s.original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(s.button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(buttonProcedure)));
        if (!s.original) { DestroyWindow(s.button); s.button = nullptr; return true; }
        observedButton.store(s.button, std::memory_order_release);
    }
    const bool enabled = s.enabled();
    if (enabled != s.lastEnabled || !IsWindowVisible(s.button)) {
        s.lastEnabled = enabled;
        SetWindowTextW(s.button, enabled ? L"Тёмный режим: включён" : L"Тёмный режим: выключен");
        InvalidateRect(s.button, nullptr, FALSE);
        if (s.settings) InvalidateRect(s.settings,nullptr,FALSE);
    }
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (s.isRetro && s.isRetro()) {
        if (s.regionWidth) {
            SetWindowRgn(s.button,nullptr,TRUE);
            s.regionWidth = s.regionHeight = 0;
        }
    } else if (s.regionWidth != width || s.regionHeight != height) {
        const int rounding = height - 2;
        if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, rounding, rounding)) {
            if (SetWindowRgn(s.button, region, TRUE)) {
                s.regionWidth = width; s.regionHeight = height;
            } else DeleteObject(region);
        }
    }
    placeControl(s.button,s.parent,rect);
    const auto gear = current.settingsRect;
    if (gear.right > gear.left) {
        if (!s.settings) {
            s.settings = CreateWindowExW(0,L"BUTTON",L"Настройки темы 1С",WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,
                gear.left,gear.top,gear.right-gear.left,gear.bottom-gear.top,s.parent,nullptr,GetModuleHandleW(nullptr),nullptr);
            if (s.settings) {
                SetWindowLongPtrW(s.settings,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&s));
                s.settingsOriginal = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(s.settings,GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(settingsProcedure)));
                if (!s.settingsOriginal) { DestroyWindow(s.settings); s.settings=nullptr; }
                observedSettings.store(s.settings,std::memory_order_release);
            }
        }
        if (s.settings) placeControl(s.settings,s.parent,gear);
    } else if (s.settings) ShowWindow(s.settings,SW_HIDE);
    // Taxi sometimes paints over child HWNDs when switching tabs. Check just
    // our own controls while the parent is foreground and repaint only damage.
    if (GetForegroundWindow()==s.parent) {
        repairIfCovered(s.button,s.switchPixels);
        repairIfCovered(s.settings,s.gearPixels);
    }
    return true;
}
inline DWORD WINAPI observer(void*) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { publish({nullptr, {}, false, true}); return 0; }
    {
        Com<IUIAutomation> automation;
        if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.put())))) {
            Observer observation{}; observation.automation = automation.value;
            while (!stopping.load(std::memory_order_acquire)) {
                // Replacing a splash/login/main HWND is normal in the thick
                // client. Keep observing until an explicit worker stop; an old
                // parent disappearing must not permanently disable the toggle.
                if (observation.parent && !IsWindow(observation.parent)) observation.parent = nullptr;
                if (state.designerMain) observation.parent=state.designerMain();
                else {
                    MainWindow found{}; EnumWindows(findMain,reinterpret_cast<LPARAM>(&found));
                    observation.parent=found.window;
                }
                observation.button = observedButton.load(std::memory_order_acquire);
                observation.settings = observedSettings.load(std::memory_order_acquire);
                RECT rectangle{}, gear{}, before{}, after{};
                if (observation.parent && GetWindowRect(observation.parent,&before)) {
                    const auto result=state.designerMain ? designerPlacement(observation,rectangle,gear) :
                        placement(observation,rectangle,gear);
                    const bool stable=GetWindowRect(observation.parent,&after) && EqualRect(&before,&after);
                    const auto last=snapshot();
                    // Keep the last verified position across a transient UIA
                    // error. The worker still expires it after five seconds.
                    if (result!=Placement::unavailable || !stable ||
                        last.parent!=observation.parent || !EqualRect(&before,&last.windowBounds)) {
                        Snapshot next{observation.parent,rectangle,stable && result==Placement::ready,false};
                        next.settingsRect=gear;
                        publish(next);
                    }
                } else publish({});
                // This thread owns no HWND, including message-only windows.
                // A blocked provider cannot block button painting or teardown.
                Sleep(1000);
            }
        }
    }
    publish({nullptr, {}, false, true});
    CoUninitialize();
    return 0;
}
inline DWORD WINAPI worker(void*) {
    // No COM/UIA calls on the window-owning thread. GDI+ lifetime belongs
    // entirely to this worker, after DLL initialization and before teardown.
    ULONG_PTR graphicsToken{};
    Gdiplus::GdiplusStartupInput graphicsInput;
    state.gdiplus = Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) == Gdiplus::Ok;
    const UINT_PTR timer = SetTimer(nullptr, 0, 250, nullptr);
    if (timer) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_TIMER && !message.hwnd && message.wParam == timer) {
                if (!tick(state)) break;
            } else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        KillTimer(nullptr, timer);
    }
    stopping.store(true, std::memory_order_release);
    discardButton(state);
    if (state.gdiplus) { state.gdiplus = false; Gdiplus::GdiplusShutdown(graphicsToken); }
    return 0;
}
} // namespace detail

// Call after the containing theme DLL is pinned and callbacks are ready. This
// starts only one worker and never waits on the application's UI thread.
inline bool start(bool (*enabled)(), void (*toggle)(), void (*refreshContent)(HWND), bool (*toggleRetro)(),
                  bool (*isRetro)(), bool (*applyProfile)(const ThemeProfile&), ThemeProfile (*profile)(),
                  HWND (*designerMain)() = nullptr) {
    if (!enabled || !toggle) return false;
    bool expected = false;
    if (!detail::started.compare_exchange_strong(expected, true)) return true;
    detail::state.enabled = enabled; detail::state.toggle = toggle;
    detail::state.refreshContent = refreshContent;
    detail::state.toggleRetro = toggleRetro;
    detail::state.isRetro = isRetro;
    detail::state.applyProfile = applyProfile;
    detail::state.profile = profile;
    detail::state.designerMain = designerMain;
    const HANDLE ui = CreateThread(nullptr, 0, detail::worker, nullptr, 0, nullptr);
    if (!ui) { detail::started.store(false); return false; }
    CloseHandle(ui);
    const HANDLE observer = CreateThread(nullptr, 0, detail::observer, nullptr, 0, nullptr);
    if (!observer) { detail::stopping.store(true, std::memory_order_release); return false; }
    CloseHandle(observer);
    return true;
}
} // namespace enterprise_toggle
