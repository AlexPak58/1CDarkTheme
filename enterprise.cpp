// Enterprise Taxi .2130: vector colors, immutable gradient copies, exact navigation
// skins and WebKit vector colors. User raster images are never recolored.
#include "platform.hpp"
#include "enterprise_navigation.hpp"
#include "enterprise_gradient.hpp"
#include "enterprise_toggle.hpp"
#pragma comment(lib, "gdi32.lib")
#include <atomic>
#include <algorithm>
#include <intrin.h>
#include <cmath>

namespace {
using Rgb = void (*)(void*, double, double, double);
using Rgba = void (*)(void*, double, double, double, double);
PVOID originalRgb{}, originalRgba{};
enterprise_navigation::PatternCreate originalPattern{};
enterprise_gradient::SetSource originalSource{};
using FillExtents = void (*)(void*,double*,double*,double*,double*);
using TransformDistance = void (*)(void*,double*,double*);
FillExtents fillExtents{};
TransformDistance deviceDistance{};
ULONG_PTR graphicsBase{};
ThemeProfile palette;
SRWLOCK profileLock = SRWLOCK_INIT;
std::atomic<bool> active{};
std::atomic<bool> retroMode{};
bool activeBeforeRetro{}; // Accessed only by callbacks on the button thread.
constexpr ThemeProfile retroPalette = [] {
    ThemeProfile result;
    auto& colors = result.colors;
    colors[workspaceBackground] = 0xF2EFE7;
    colors[propertyBackground] = 0xFFFFFF;
    colors[propertyText] = 0x101015;
    colors[propertyMutedText] = 0x343C4D;
    colors[propertySelection] = 0x729BD0;
    colors[barsBackground] = 0xAFC3DE;
    colors[barsText] = 0x141B2C;
    colors[panelBorder] = 0x706C64;
    return result;
}();
ThemeProfile currentPalette(bool* retro = nullptr) {
    const bool retroEnabled = retroMode.load(std::memory_order_acquire);
    if (retro) *retro = retroEnabled;
    if (retroEnabled) return retroPalette;
    AcquireSRWLockShared(&profileLock);
    const auto snapshot = palette;
    ReleaseSRWLockShared(&profileLock);
    return snapshot;
}
bool validProfile(const ThemeProfile& profile) {
    return profile.size == sizeof(ThemeProfile) && profile.version == themeProfileVersion &&
        profile.zones <= themeZoneMask &&
        std::all_of(profile.colors.begin(),profile.colors.end(),[](DWORD color) { return color <= 0xFFFFFF; });
}
bool applyLiveProfile(const ThemeProfile& profile) {
    if (!validProfile(profile)) return false;
    AcquireSRWLockExclusive(&profileLock);
    if (profile.zones) palette = profile;
    ReleaseSRWLockExclusive(&profileLock);
    retroMode.store(false,std::memory_order_release);
    active.store(profile.zones != 0,std::memory_order_release);
    return true;
}
SRWLOCK lock = SRWLOCK_INIT;
bool installed{};
bool configured{};
std::atomic<DWORD> htmlStatus{ERROR_NOT_READY};
std::atomic<bool> htmlReady{};
constexpr ULONG_PTR webViewProcedureRva = 0x1109F0;
std::atomic<ULONG_PTR> webViewProcedure{};

BOOL CALLBACK refreshHtmlChild(HWND window, LPARAM parameter) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t name[32]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        std::wstring_view(name) == L"WebViewWindowClass" &&
        GetClassLongPtrW(window, GCLP_WNDPROC) == static_cast<ULONG_PTR>(parameter)) {
        // WebKit's native theme handler discards its backing store on the
        // owning thread, then invalidates the view without reloading the page.
        PostMessageW(window, WM_THEMECHANGED, 0, 0);
    }
    return TRUE;
}
void refreshHtml(HWND root) {
    if (!htmlReady.load(std::memory_order_acquire)) return;
    const auto procedure = webViewProcedure.load(std::memory_order_relaxed);
    DWORD pid{};
    if (procedure && GetWindowThreadProcessId(root, &pid) && pid == GetCurrentProcessId())
        EnumChildWindows(root, &refreshHtmlChild, static_cast<LPARAM>(procedure));
}
bool themeEnabled() { return active.load(std::memory_order_acquire); }
void switchTheme() {
    retroMode.store(false,std::memory_order_release);
    auto previous=active.load(std::memory_order_relaxed);
    while (!active.compare_exchange_weak(previous,!previous,std::memory_order_release,std::memory_order_relaxed)) {}
}
bool switchRetro() {
    if (retroMode.load(std::memory_order_relaxed)) {
        retroMode.store(false,std::memory_order_release);
        active.store(activeBeforeRetro,std::memory_order_release);
        return false;
    }
    activeBeforeRetro = themeEnabled();
    retroMode.store(true,std::memory_order_release);
    active.store(true,std::memory_order_release);
    return true;
}

bool channels(double r, double g, double b) {
    return r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1;
}
void unpack(DWORD rgb, double& r, double& g, double& b) {
    r = ((rgb >> 16) & 255) / 255.0;
    g = ((rgb >> 8) & 255) / 255.0;
    b = (rgb & 255) / 255.0;
}
double relativeLuminance(double r,double g,double b);
bool lightPalette(const ThemeProfile& profile) {
    const auto color = profile.colors[workspaceBackground];
    return ((color >> 16) & 255) >= 128 && ((color >> 8) & 255) >= 128 && (color & 255) >= 128;
}
// The stock Taxi chrome and search have observed pale yellow base colors.
// Keep their treatment narrow so other yellows retain their semantic hue.
bool stockTaxiYellow(double r, double g, double b) {
    if (r < .97) return false;
    const double green = g / r, blue = b / r;
    return (green >= .936 && green <= .960 && blue >= .616 && blue <= .648) ||
           (green >= .965 && green <= .995 && blue >= .835 && blue <= .875);
}
bool nearRgb(double r, double g, double b, DWORD color) {
    double expectedR{}, expectedG{}, expectedB{};
    unpack(color, expectedR, expectedG, expectedB);
    constexpr double tolerance = 1.5 / 255;
    return std::fabs(r-expectedR) <= tolerance &&
        std::fabs(g-expectedG) <= tolerance && std::fabs(b-expectedB) <= tolerance;
}
void limitLuminance(double& r,double& g,double& b,double target) {
    if (relativeLuminance(r,g,b)<=target) return;
    double lower=0,upper=1;
    for (int step=0;step<24;++step) {
        const double factor=(lower+upper)*.5;
        if (relativeLuminance(r*factor,g*factor,b*factor)>target) upper=factor;
        else lower=factor;
    }
    r*=lower;g*=lower;b*=lower;
}
bool editSelection(double& r,double& g,double& b,double alpha) {
    // The Taxi edit paints #536AC2 as an opaque vector under its glyphs.
    if (alpha != 1 || !nearRgb(r,g,b,0x536AC2)) return false;
    const auto profile=currentPalette();
    unpack(profile.colors[propertySelection],r,g,b);
    double foreground=1;
    if (!lightPalette(profile)) {
        double tr{},tg{},tb{};
        unpack(profile.colors[propertyText],tr,tg,tb);
        foreground=relativeLuminance(tr,tg,tb);
    }
    limitLuminance(r,g,b,(foreground+.05)/4.8-.05);
    return true;
}
bool taxiFocusAccent(double red, double green, double blue) {
    // The amber used by Taxi's focused list item and field border has a
    // different hue from the gold action-button gradients.
    return red >= .70 && green >= red*.785 && green <= red*.84 &&
        blue >= red*.08 && blue <= red*.15;
}
void blendSurface(double& r,double& g,double& b,DWORD baseColor,DWORD accent,double amount) {
    double red{},green{},blue{};
    unpack(baseColor,r,g,b);
    unpack(accent,red,green,blue);
    r+=(red-r)*amount;g+=(green-g)*amount;b+=(blue-b)*amount;
}
void raiseLuminance(double& r,double& g,double& b,double target) {
    if (relativeLuminance(r,g,b)>=target) return;
    double lower=0,upper=1;
    for (int step=0;step<40;++step) {
        const double factor=(lower+upper)*.5;
        if (relativeLuminance(r+(1-r)*factor,g+(1-g)*factor,b+(1-b)*factor)<target) lower=factor;
        else upper=factor;
    }
    r+=(1-r)*upper;g+=(1-g)*upper;b+=(1-b)*upper;
}
// Use one monotonic luminance transform for every chromatic fill. Scaling RGB
// together preserves hue, saturation and distinct source shades; no blue/yellow
// bucket may replace a business highlight with the common selection/chrome.
void surface(double& r, double& g, double& b, bool compact = false) {
    if (!channels(r,g,b)) return;
    bool retro{};
    const auto profile = currentPalette(&retro);
    const double hi = (std::max)(r,(std::max)(g,b));
    const double lo = (std::min)(r,(std::min)(g,b));
    if (!lightPalette(profile) && taxiFocusAccent(r,g,b)) {
        blendSurface(r,g,b,profile.colors[workspaceBackground],profile.colors[propertySelection],.86);
    } else if (hi-lo <= 1.0/255.0) {
        if (lo > .72) {
            // The list uses white and near-white bands with a separate grey header.
            const double lift = lo >= 248.0/255 && lo <= 251.0/255 ? .031 :
                lo >= 239.0/255 && lo <= 244.0/255 ? .055 : 0;
            if (compact && lightPalette(profile)) {
                // Plain labels and read-only totals also use compact white
                // rectangles. Keep them close to the document surface.
                blendSurface(r,g,b,profile.colors[workspaceBackground],profile.colors[propertyBackground],.18);
                return;
            }
            unpack(profile.colors[compact ? propertyBackground : workspaceBackground],r,g,b);
            if (retro && !compact) {
                if (lo >= 252.0/255) unpack(0xFFFFFF,r,g,b);
                else if (lo >= 248.0/255) unpack(0xF4F7FB,r,g,b);
                else if (lo <= 244.0/255) unpack(0xD4D0C8,r,g,b);
                return;
            }
            r+=(1-r)*lift;g+=(1-g)*lift;b+=(1-b)*lift;
        }
    } else if (nearRgb(r,g,b,0xFEF2C7) || nearRgb(r,g,b,0xFDE68F)) {
        // Preserve the distinction between the selected row and its focused cell.
        const bool focused = b < .65;
        unpack(profile.colors[propertySelection],r,g,b);
        if (!focused) {
            if (lightPalette(profile)) { r+=(1-r)*.25;g+=(1-g)*.25;b+=(1-b)*.25; }
            else { r*=.77;g*=.77;b*=.77; }
        }
    } else if (stockTaxiYellow(r,g,b)) {
        const bool search = b / r > .8;
        unpack(profile.colors[search ? propertyBackground : barsBackground],r,g,b);
        const double shade = lightPalette(profile) ? 1 : search ? .91 : .76;
        r*=shade;g*=shade;b*=shade;
    } else if (lightPalette(profile)) {
        // Tinted fills keep their hue above the dark palette text.
        const double mix = retro && lo >= .5 ? .42 : lo < .5 ? .65 : .25;
        r+=(1-r)*mix;g+=(1-g)*mix;b+=(1-b)*mix;
    } else {
        double textR{},textG{},textB{};
        unpack(profile.colors[propertyText],textR,textG,textB);
        const auto readable=(relativeLuminance(textR,textG,textB)+.05)/4.5-.05;
        const double target=relativeLuminance(r,g,b)*std::clamp(readable,0.0,.12);
        double lower=0,upper=1;
        for (int step=0;step<40;++step) {
            const double factor=(lower+upper)*.5;
            if (relativeLuminance(r*factor,g*factor,b*factor)>target) upper=factor;
            else lower=factor;
        }
        r*=lower;g*=lower;b*=lower;
    }
}
void outline(double& r, double& g, double& b) {
    if (!channels(r,g,b)) return;
    bool retro{};
    const auto profile = currentPalette(&retro);
    const double hi=(std::max)(r,(std::max)(g,b)), lo=(std::min)(r,(std::min)(g,b));
    if (!lightPalette(profile) && taxiFocusAccent(r,g,b)) {
        blendSurface(r,g,b,profile.colors[propertySelection],profile.colors[panelBorder],.55);
        return;
    }
    if (retro && hi-lo > .06 && r >= .65 && g >= .5 && r > b+.10) {
        unpack(0x335783,r,g,b);
        return;
    }
    if (hi-lo > 1.0/255.0) {
        raiseLuminance(r,g,b,(.12+.05)*3-.05);return;
    }
    if (profile.colors[panelBorder] != ThemeProfile{}.colors[panelBorder]) {
        unpack(profile.colors[panelBorder],r,g,b);
        return;
    }
    double br{},bg{},bb{},fr{},fg{},fb{};
    unpack(profile.colors[workspaceBackground],br,bg,bb);
    unpack(profile.colors[propertyMutedText],fr,fg,fb);
    r=br*.45+fr*.55;g=bg*.45+fg*.55;b=bb*.45+fb*.55;
    // Grid strokes must contrast with conditional fills as well as the canvas.
    raiseLuminance(r,g,b,(.12+.05)*3-.05);
}
bool thinFill(void* cr) {
    double x1{},y1{},x2{},y2{};
    fillExtents(cr,&x1,&y1,&x2,&y2);
    double wx=x2-x1,wy{},hx{},hy=y2-y1,sx=1,sy{},tx{},ty=1;
    if (!std::isfinite(wx) || !std::isfinite(hy) || wx<=0 || hy<=0) return false;
    deviceDistance(cr,&wx,&wy);deviceDistance(cr,&hx,&hy);
    deviceDistance(cr,&sx,&sy);deviceDistance(cr,&tx,&ty);
    // Tables use axis-aligned paths. Do not reinterpret rotated charts as grid.
    if (std::fabs(sy)>1e-6 || std::fabs(tx)>1e-6) return false;
    const double width=std::hypot(wx,wy),height=std::hypot(hx,hy);
    const double unitX=std::hypot(sx,sy),unitY=std::hypot(tx,ty);
    return std::isfinite(width) && std::isfinite(height) && std::isfinite(unitX) && std::isfinite(unitY) &&
        unitX>0 && unitY>0 && width>0 && height>0 && width<1e6 && height<1e6 &&
        ((width<=1.25*unitX && height>=3*unitY) || (height<=1.25*unitY && width>=3*unitX));
}
bool compactControlFill(void* cr) {
    const auto& profile = currentPalette();
    if (profile.colors[propertyBackground] == profile.colors[workspaceBackground]) return false;
    double left{},top{},right{},bottom{};
    fillExtents(cr,&left,&top,&right,&bottom);
    const auto width=right-left, height=bottom-top;
    return width>=55 && width<=1000 && height>=16 && height<=32;
}
double relativeLuminance(double r,double g,double b) {
    const auto linear=[](double value) {return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4);};
    return .2126*linear(r)+.7152*linear(g)+.0722*linear(b);
}
void text(double& r, double& g, double& b, bool glyph = false) {
    if (!channels(r,g,b)) return;
    bool retro{};
    const auto profile = currentPalette(&retro);
    const double hi = (std::max)(r,(std::max)(g,b));
    const double lo = (std::min)(r,(std::min)(g,b));
    if (retro && glyph && hi < .98 && hi-lo <= .16) {
        // Placeholder and suppressed labels may reach the glyph route as
        // nearly white. Keep them legible on the paper and silver surfaces.
        unpack(profile.colors[hi > .45 ? propertyMutedText : propertyText],r,g,b);
    } else if (hi-lo <= 1.0/255.0 && (hi < .72 || (glyph && hi < .98))) {
        // Disabled values and search hints can arrive above the normal text
        // cutoff. Preserve solid white used over the selected edit background.
        unpack(profile.colors[hi > .45 ? propertyMutedText : propertyText],r,g,b);
    }
    else if (glyph && hi-lo > 1.0/255.0) {
        double br{},bg{},bb{};
        unpack(profile.colors[workspaceBackground],br,bg,bb);
        if (lightPalette(profile)) {
            const double target = (relativeLuminance(br,bg,bb)+.05)/4.5-.05;
            limitLuminance(r,g,b,target);
            return;
        }
        // Saturated red labels in the list sit on neutral bands. A worst-case
        // target for every hue bleaches them to pinkish white.
        const bool saturatedRed = r >= .55 && r >= g+.35 && r >= b+.25;
        const auto back=(std::max)(saturatedRed ? .045 : .12,relativeLuminance(br,bg,bb)), front=relativeLuminance(r,g,b);
        const auto contrast=((std::max)(back,front)+.05)/((std::min)(back,front)+.05);
        if (contrast<4.5) {
            raiseLuminance(r,g,b,(back+.05)*4.5-.05);
        }
    }
}
void vectorColor(double& r, double& g, double& b) {
    const double hi=(std::max)(r,(std::max)(g,b)),lo=(std::min)(r,(std::min)(g,b));
    // Unknown vector paths include text as well as surfaces. Confirmed fill
    // callers use surface directly; retain the conservative routing here.
    if (hi-lo>1.0/255.0 && lo<=.60 && !(r>=.80 && g>=.55 && b<.40 && g<=r)) {
        text(r,g,b,true);return;
    }
    const auto oldR=r, oldG=g, oldB=b;
    surface(r,g,b);
    if (r == oldR && g == oldG && b == oldB) text(r,g,b);
}
void hookedRgb(void* cr, double r, double g, double b) {
    if (active.load(std::memory_order_acquire)) {
        // AEA00 tail-calls RGB after restoring its stack: this is its stroke caller.
        const auto caller=reinterpret_cast<ULONG_PTR>(_ReturnAddress());
        if (caller == graphicsBase+0xAE4DD || (caller == graphicsBase+0xAE586 && thinFill(cr))) outline(r,g,b);
        else surface(r,g,b,caller == graphicsBase+0xAE586 && compactControlFill(cr));
    }
    reinterpret_cast<Rgb>(originalRgb)(cr,r,g,b);
}
void hookedRgba(void* cr, double r, double g, double b, double a) {
    if (active.load(std::memory_order_acquire)) {
        const auto* stack = static_cast<const ULONG_PTR*>(_AddressOfReturnAddress());
        const auto top = reinterpret_cast<ULONG_PTR>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
        const auto address = reinterpret_cast<ULONG_PTR>(stack);
        // grphcs+2336 converts ARGB; its caller at stack+40 is the glyph path.
        const bool glyph = reinterpret_cast<ULONG_PTR>(_ReturnAddress()) == graphicsBase + 0x2336 &&
            address <= top && top - address >= 0x48 && stack[0x40 / 8] == graphicsBase + 0x78F51;
        const auto caller=reinterpret_cast<ULONG_PTR>(_ReturnAddress());
        const bool stroke=(caller == graphicsBase+0xAEA59 && address <= top && top-address >= 0x48 &&
             stack[0x40/8] == graphicsBase+0xAE4DD);
        const bool lineFill=caller == graphicsBase+0xAEA59 && address<=top && top-address>=0x48 &&
            stack[0x40/8] == graphicsBase+0xAE586 && thinFill(cr);
        if (caller == graphicsBase+0x2336 && !glyph && editSelection(r,g,b,a)) {
            reinterpret_cast<Rgba>(originalRgba)(cr,r,g,b,a);
            return;
        }
        if (glyph || caller == graphicsBase+0x73E6 || caller == graphicsBase+0x744A) text(r,g,b,true);
        else if (stroke || lineFill) outline(r,g,b);
        else if (caller == graphicsBase + 0xAEA59) surface(r,g,b,
            address <= top && top-address >= 0x48 && stack[0x40/8] == graphicsBase+0xAE586 &&
            compactControlFill(cr));
        else vectorColor(r,g,b);
    }
    reinterpret_cast<Rgba>(originalRgba)(cr,r,g,b,a);
}
void gradientColor(double& r, double& g, double& b) {
    if (channels(r,g,b)) {
        const double hi = (std::max)(r,(std::max)(g,b));
        const double lo = (std::min)(r,(std::min)(g,b));
        if (r>=.75 && g>=r*.84 && g<=r*.96 && b>=r*.07 && b<=r*.24) {
            const auto profile=currentPalette();
            if (!lightPalette(profile)) {
                const double brightness=r;
                blendSurface(r,g,b,profile.colors[workspaceBackground],0x665135,.34+brightness*.23);
                return;
            }
        }
        if (retroMode.load(std::memory_order_acquire) && hi-lo <= 1.0/255.0 && lo >= .68) {
            // Preserve a distinct highlight and shadow in stock control bevels.
            // Separate the dark edge, silver face and white highlight to
            // emphasize the stock control's existing raised geometry.
            if (lo >= .965) unpack(0xFAF9F4,r,g,b);
            else if (lo >= .86) unpack(0xD4D0C8,r,g,b);
            else unpack(0x8E8980,r,g,b);
            return;
        }
        // Light neutral/pastel stops and the separately handled gold accent.
        if (hi-lo>1.0/255.0 || lo>.60) surface(r,g,b);
    }
}
void htmlRgb(void* cr, double r, double g, double b) {
    if (htmlReady.load(std::memory_order_acquire) && active.load()) vectorColor(r,g,b);
    reinterpret_cast<Rgb>(originalRgb)(cr,r,g,b);
}
void htmlRgba(void* cr, double r, double g, double b, double a) {
    if (htmlReady.load(std::memory_order_acquire) && active.load()) vectorColor(r,g,b);
    reinterpret_cast<Rgba>(originalRgba)(cr,r,g,b,a);
}
void htmlSource(void* cr,void* pattern) {
    if (htmlReady.load(std::memory_order_acquire) && active.load())
        enterprise_gradient::setSource(originalSource,cr,pattern,&gradientColor);
    else originalSource(cr,pattern);
}
void* themedImage(void* surface) {
    return enterprise_navigation::recolor(surface,currentPalette(),true);
}
void gradientSource(void* cr,void* pattern) {
    if (active.load(std::memory_order_acquire)) {
        // Taxi scales stock filter skins at this source-selection call,
        // including patterns cached before the theme was attached.
        const bool controlSkin=reinterpret_cast<ULONG_PTR>(_ReturnAddress())==graphicsBase+0x13724;
        enterprise_gradient::setSource(originalSource,cr,pattern,&gradientColor,nullptr,
                                       controlSkin ? &themedImage : nullptr);
    }
    else originalSource(cr,pattern);
}
DWORD installHtml() {
    HMODULE module{};
    if (!GetModuleHandleExW(0,L"WebKit.dll",&module)) return ERROR_MOD_NOT_FOUND;
    struct ReleaseModule { HMODULE module; ~ReleaseModule() { FreeLibrary(module); } } held{module};
    if (!matchesSha256(modulePath(module),
        L"D8AC7240222CC741794308A92FB01D2AF04E9134ABB1215666999D52703F026C"))
        return ERROR_REVISION_MISMATCH;
    const auto base = reinterpret_cast<ULONG_PTR>(module);
    struct Hook { ULONG_PTR rva; PVOID expected, replacement; };
    const Hook hooks[] = {
        {0x11DB328, originalRgba, reinterpret_cast<PVOID>(&htmlRgba)},
        {0x11DB4B0, originalRgb, reinterpret_cast<PVOID>(&htmlRgb)},
        {0x11DB5F0, reinterpret_cast<PVOID>(originalSource), reinterpret_cast<PVOID>(&htmlSource)}
    };
    for (const auto& hook : hooks)
        if (*reinterpret_cast<PVOID volatile*>(base + hook.rva) != hook.expected)
            return ERROR_INVALID_DATA;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &pinned)) return GetLastError();
    auto* start = reinterpret_cast<PVOID*>(base + hooks[0].rva);
    constexpr SIZE_T length = 0x11DB5F0 - 0x11DB328 + sizeof(PVOID);
    DWORD protection{};
    if (!VirtualProtect(start,length,PAGE_READWRITE,&protection)) return GetLastError();
    size_t changed{};
    for (const auto& hook : hooks) {
        if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(base + hook.rva),
            hook.replacement,hook.expected) != hook.expected) break;
        ++changed;
    }
    DWORD result = changed == std::size(hooks) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    if (result) while (changed) {
        const auto& hook = hooks[--changed];
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(base + hook.rva),
            hook.expected,hook.replacement);
    }
    DWORD ignored{};
    if (!VirtualProtect(start,length,protection,&ignored) && !result) result=GetLastError();
    if (!result) {
        webViewProcedure.store(base + webViewProcedureRva, std::memory_order_relaxed);
        htmlReady.store(true,std::memory_order_release);
    }
    return result;
}
DWORD WINAPI waitForHtml(LPVOID) {
    for (;;) {
        const auto result=installHtml();
        if (result != ERROR_MOD_NOT_FOUND) {
            htmlStatus.store(result);
            if (result == ERROR_SUCCESS && themeEnabled()) {
                // Automatic attach can arrive after the news view has cached
                // its first light frame. Refresh existing views on their UI thread.
                EnumWindows([](HWND window, LPARAM) -> BOOL {
                    refreshHtml(window);
                    return TRUE;
                },0);
            }
            return result;
        }
        Sleep(500);
    }
}
void* navigationPattern(void* source) {
    if (active.load(std::memory_order_acquire))
        return enterprise_navigation::patternForSurface(originalPattern,source,currentPalette());
    return originalPattern(source);
}
DWORD install() {
    const auto graphics = GetModuleHandleW(L"grphcs.dll");
    const auto cairo = GetModuleHandleW(L"cairo.dll");
    if (!graphics || !cairo) return ERROR_MOD_NOT_FOUND;
    if ((!matchesSha256(modulePath(nullptr),exeHash) && !matchesSha256(modulePath(nullptr),thinClientHash)) ||
        !matchesSha256(modulePath(graphics),graphicsHash) || !matchesSha256(modulePath(cairo),cairoHash)) return ERROR_REVISION_MISMATCH;
    graphicsBase = reinterpret_cast<ULONG_PTR>(graphics);
    auto* source = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7B08);
    auto* rgb = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7B60);
    auto* rgba = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7C20);
    auto* pattern = reinterpret_cast<PVOID volatile*>(graphicsBase + 0xC7BE0);
    originalRgb = reinterpret_cast<PVOID>(GetProcAddress(cairo,"cairo_set_source_rgb"));
    originalRgba = reinterpret_cast<PVOID>(GetProcAddress(cairo,"cairo_set_source_rgba"));
    originalSource = reinterpret_cast<enterprise_gradient::SetSource>(GetProcAddress(cairo,"cairo_set_source"));
    originalPattern = reinterpret_cast<enterprise_navigation::PatternCreate>(GetProcAddress(cairo,"cairo_pattern_create_for_surface"));
    fillExtents=reinterpret_cast<FillExtents>(GetProcAddress(cairo,"cairo_fill_extents"));
    deviceDistance=reinterpret_cast<TransformDistance>(GetProcAddress(cairo,"cairo_user_to_device_distance"));
    if (!originalRgb || !originalRgba || !originalSource || *rgb != originalRgb ||
        !originalPattern || !fillExtents || !deviceDistance || *rgba != originalRgba ||
        *source != reinterpret_cast<PVOID>(originalSource) ||
        *pattern != reinterpret_cast<PVOID>(originalPattern)) return ERROR_INVALID_DATA;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&hookedRgb),&pinned)) return GetLastError();
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(cairo),&pinned)) return GetLastError();
    if (!enterprise_navigation::initialize(cairo)) return ERROR_PROC_NOT_FOUND;
    if (!enterprise_gradient::initialize(cairo)) return ERROR_PROC_NOT_FOUND;
    DWORD protection{};
    auto* start = const_cast<PVOID*>(source);
    constexpr SIZE_T size = 0xC7C20 - 0xC7B08 + sizeof(PVOID);
    if (!VirtualProtect(start,size,PAGE_READWRITE,&protection)) return GetLastError();
    const auto beforeRgb = InterlockedCompareExchangePointer(rgb,reinterpret_cast<PVOID>(&hookedRgb),originalRgb);
    const auto beforeRgba = InterlockedCompareExchangePointer(rgba,reinterpret_cast<PVOID>(&hookedRgba),originalRgba);
    const auto beforeSource = InterlockedCompareExchangePointer(source,reinterpret_cast<PVOID>(&gradientSource),
        reinterpret_cast<PVOID>(originalSource));
    const auto beforePattern = InterlockedCompareExchangePointer(pattern,reinterpret_cast<PVOID>(&navigationPattern),
        reinterpret_cast<PVOID>(originalPattern));
    DWORD error = ERROR_SUCCESS;
    if (beforeRgb != originalRgb || beforeRgba != originalRgba || beforeSource != reinterpret_cast<PVOID>(originalSource) ||
        beforePattern != reinterpret_cast<PVOID>(originalPattern)) {
        if (beforePattern == reinterpret_cast<PVOID>(originalPattern)) InterlockedCompareExchangePointer(pattern,
            reinterpret_cast<PVOID>(originalPattern),reinterpret_cast<PVOID>(&navigationPattern));
        if (beforeSource == reinterpret_cast<PVOID>(originalSource)) InterlockedCompareExchangePointer(source,
            reinterpret_cast<PVOID>(originalSource),reinterpret_cast<PVOID>(&gradientSource));
        if (beforeRgb == originalRgb) InterlockedCompareExchangePointer(rgb,originalRgb,reinterpret_cast<PVOID>(&hookedRgb));
        if (beforeRgba == originalRgba) InterlockedCompareExchangePointer(rgba,originalRgba,reinterpret_cast<PVOID>(&hookedRgba));
        error = ERROR_INVALID_DATA;
    }
    DWORD ignored{};
    if (!VirtualProtect(start,size,protection,&ignored) && !error) error = GetLastError();
    installed = !error;
    return error;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI EnterpriseThemeProfileAttachV11(LPVOID) {
    return themeProfileVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI SetThemeProfile(LPVOID argument) {
    if (!argument) return ERROR_INVALID_PARAMETER;
    const auto& requested = *static_cast<const ThemeProfile*>(argument);
    if (!validProfile(requested)) return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    DWORD result = ERROR_ALREADY_ASSIGNED;
    if (!configured) {
        result = installed ? ERROR_SUCCESS : install();
        if (!result) {
            AcquireSRWLockExclusive(&profileLock);
            palette = requested;
            ReleaseSRWLockExclusive(&profileLock);
            configured = true;
            active.store(requested.zones != 0,std::memory_order_release);
            if (requested.zones && htmlStatus.load() == ERROR_NOT_READY) {
                const auto worker=CreateThread(nullptr,0,&waitForHtml,nullptr,0,nullptr);
                if (worker) CloseHandle(worker);
                else htmlStatus.store(GetLastError());
            }
            if (requested.zones) enterprise_toggle::start(&themeEnabled,&switchTheme,&refreshHtml,&switchRetro,
                [] { return retroMode.load(std::memory_order_acquire); },&applyLiveProfile,
                [] { return currentPalette(); });
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
