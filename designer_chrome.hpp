#pragma once

#include "enterprise_gradient.hpp"
#include "enterprise_navigation.hpp"
#include "scroller_png_hook.hpp"
#include <algorithm>
#include <intrin.h>

// The Configurator and the Enterprise client use the same pinned Cairo ABI.
// These routes are only installed after the Designer's own PE admission.
namespace designer_chrome {
namespace detail {
using Paint = void (*)(void*);
struct Glyph { unsigned long index; double x,y; };
using Show = void (*)(void*,const Glyph*,int);
using Source = enterprise_gradient::SetSource;
struct Binding {
    ULONG_PTR frame{}, graphics{};
    const std::atomic<const ThemeProfile*>* profile{};
    Paint fill{}, stroke{}, save{}, restore{};
    Show show{};
    Source source{};
    void* (*getSource)(void*){};
    int (*rgba)(void*,double*,double*,double*,double*){};
    void (*setRgb)(void*,double,double,double){};
    void (*fillExtents)(void*,double*,double*,double*,double*){};
};
inline Binding binding;
inline bool installed{};

using Calls=std::array<ULONG_PTR,16>;
inline bool startsWith(const Calls& calls,std::initializer_list<ULONG_PTR> prefix) {
    return std::equal(prefix.begin(),prefix.end(),calls.begin());
}
inline Calls frameCalls() {
    void* stack[56]{};
    const auto depth=CaptureStackBackTrace(1,static_cast<DWORD>(std::size(stack)),stack,nullptr);
    Calls result{};
    size_t count{};
    for (USHORT i=0;i<depth && count<result.size();++i) {
        const auto address=reinterpret_cast<ULONG_PTR>(stack[i]);
        if (address>=binding.frame && address-binding.frame<0xB00000)
            result[count++]=address-binding.frame;
    }
    return result;
}
inline bool mainDock(const Calls& calls, size_t offset) {
    return calls[offset]==0x8D5C70 && calls[offset+1]==0x8D3581;
}
inline bool chapterDialog(const Calls& calls,size_t offset) {
    return offset+5<calls.size() && mainDock(calls,offset) &&
            calls[offset+2]==0x628382 && calls[offset+3]==0x639C5B &&
            calls[offset+4]==0x638BBB && calls[offset+5]==0x946F72;
}
inline bool dockList(const Calls& calls,size_t offset) {
    return mainDock(calls,offset) &&
           std::all_of(calls.begin()+offset+2,calls.end(),[](ULONG_PTR call){ return !call; });
}
inline bool settingsDialog(const Calls& calls,size_t offset) {
    return calls[offset]==0xBC58D && calls[offset+1]==0xBC5C5 && calls[offset+2]==0xBC6D3;
}
inline bool modalContent(const Calls& calls,size_t offset) {
    if (settingsDialog(calls,offset)) return true;
    if (!mainDock(calls,offset)) return false;
    const auto owner=calls[offset+2];
    const auto entry=calls[offset+4];
    const bool direct=entry==0x95F2CE && calls[offset+5]==0x946F72;
    return (owner==0x627774 && calls[offset+3]==0x638D8C && (entry==0x7B0D70 || direct)) ||
           (owner==0x628382 && calls[offset+3]==0x639C5B &&
            (entry==0x921627 || direct || (entry==0x638BBB && calls[offset+5]==0x7F068A)));
}
inline bool editChrome(const Calls& calls, ULONG_PTR route) {
    return calls[0]==route && calls[1]==0x38633 && calls[2]==0x21322D &&
        (mainDock(calls,3) || settingsDialog(calls,3));
}
inline bool tabs(const Calls& calls, ULONG_PTR route, size_t index) {
    return calls[0]==route && (calls[index]==0x5D3ED9 || calls[index]==0x5D3F15) &&
        calls[index+1]==0x5D347E && calls[index+2]==0x5CE319;
}
inline DWORD mix(DWORD a,DWORD b,unsigned amount) {
    DWORD color{};
    for (unsigned shift=0;shift<24;shift+=8)
        color|=((((a>>shift)&255)*(100-amount)+((b>>shift)&255)*amount+50)/100)<<shift;
    return color;
}
constexpr DWORD unchanged=0xFFFFFFFF;
inline bool sheetHeaderColor(DWORD rgb) {
    return rgb==0xF7F5E6 || rgb==0xF8F6E7 || rgb==0xF9F7E8 || rgb==0xFAF8E9 ||
           rgb==0xFBF9EA || rgb==0xFCFAEB || rgb==0xFDFBEC || rgb==0xFEFCED ||
           rgb==0xFFFDEE || rgb==0xFFFEEF || rgb==0xFFFFF0;
}
inline bool sheetHeaderOnScreen(const Calls& calls,ULONG_PTR frameBase) {
    const auto start=(calls[2]==0x1B547A) ? 3u : 4u;
    return calls[1]==0x10F809 &&
        (calls[2]==0x1B547A || (calls[2]==0x1B5AE9 && calls[3]==0x1B577E)) &&
        calls[start]==0xE5772 && calls[start+1]==0xD3F3D &&
        calls[start+2]==frameBase+0x8D5C70 && calls[start+3]==frameBase+0x8D3581;
}
inline DWORD sheetHeaderPaint(ULONG_PTR route,DWORD rgb,const Calls& calls,ULONG_PTR frameBase,
                              const ThemeProfile& profile) {
    if (!(profile.zones&enableTreeAndProperties) || !sheetHeaderOnScreen(calls,frameBase))
        return unchanged;
    if ((route==0x78F95 || route==0x7900F) && rgb==0x413003 &&
        (calls[0]==0x1050A6 || calls[0]==0x105704)) return profile.colors[propertyText];
    if (calls[0]!=0x105028 && calls[0]!=0x105628 && calls[0]!=0x105AE8) return unchanged;
    if (route==0xAE4EA && rgb==0xB3AC86) return profile.colors[panelBorder];
    if (route==0xAE795 && sheetHeaderColor(rgb))
        return mix(profile.colors[barsBackground],profile.colors[panelBorder],12);
    return unchanged;
}
inline Calls sheetPaintCalls() {
    // On-demand because the tabular-document renderer may load after Designer starts.
    static std::atomic<ULONG_PTR> renderer{};
    auto base=renderer.load(std::memory_order_acquire);
    if (!base) {
        HMODULE current{};
        if (!GetModuleHandleExW(0,L"moxelui.dll",&current)) return {};
        if (matchesSha256(modulePath(current),
                L"6FBD6E3238D5FFCE7A61E9F6C52917F1D65BDD616763C2E265C512986C13EDCD")) {
            HMODULE pinned{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(current),&pinned)) base=reinterpret_cast<ULONG_PTR>(pinned);
        }
        FreeLibrary(current);
        if (!base) base=1;
        renderer.store(base,std::memory_order_release);
    }
    if (base==1) return {};
    void* stack[42]{};
    const auto depth=CaptureStackBackTrace(1,static_cast<DWORD>(std::size(stack)),stack,nullptr);
    Calls result{};
    size_t count{};
    for (USHORT i=0;i<depth && count<result.size();++i) {
        const auto address=reinterpret_cast<ULONG_PTR>(stack[i]);
        if (address>=base && address-base<0x250000)
            result[count++]=address-base;
        else if (address>=binding.frame && address-binding.frame<0xB00000)
            result[count++]=address;
    }
    return result;
}
inline bool dialogTabs(const Calls& calls,ULONG_PTR route,size_t index) {
    return calls[0]==route && (calls[index]==0x5D3ED9 || calls[index]==0x5D3F15) &&
        calls[index+1]==0x5D347E && calls[index+2]==0x5D1515 && calls[index+3]==0x3E793C &&
        calls[index+4]==0x3E6FC3;
}
inline bool button(const Calls& calls,size_t index) {
    return calls[index]==0x2A5BB && (calls[index+1]==0x2A993 || calls[index+1]==0x2AB37) &&
        calls[index+2]==0x2A33D && calls[index+3]==0x38633 &&
        calls[index+4]==0x28637 && calls[index+5]==0x388CD;
}
inline bool dialogListCell(const Calls& calls) {
    return startsWith(calls,{0x85F445,0x812B2B,0x303E26,0x303167,0x811C25,0x811A26,
                             0x8117C5,0x3666FF,0x38633,0x261A67}) &&
           (modalContent(calls,10) || chapterDialog(calls,10) || dockList(calls,10));
}
inline bool helpToolbarText(const Calls& calls) {
    return startsWith(calls,{0x7347DF,0x739469,0x712498,0x70FC6C,0x74EBA4,0x70FB75,
                             0x50CAE,0x50A15}) && mainDock(calls,8);
}
inline bool gridHeader(const Calls& calls) {
    return (calls[0]==0x811725 || calls[0]==0x8116C4) &&
        calls[1]==0x810A94 && calls[2]==0x3028DA && calls[3]==0x80FE6A &&
        calls[4]==0x811A4D && calls[5]==0x8117C5 && calls[6]==0x3666FF &&
        calls[7]==0x38633 && calls[8]==0x261A67;
}
inline bool gridHeaderSurface(const Calls& calls) {
    return (calls[0]==0x8139C3 || calls[0]==0x813A58) &&
        calls[1]==0x8104B0 && calls[2]==0x3028DA && calls[3]==0x80FE6A &&
        calls[4]==0x811A4D && calls[5]==0x8117C5 && calls[6]==0x3666FF &&
        calls[7]==0x38633 && calls[8]==0x261A67;
}
inline DWORD dialogText(DWORD rgb,const Calls& calls,const ThemeProfile& profile) {
    const auto& colors=profile.colors;
    if (rgb==0x594304 && gridHeader(calls)) return colors[propertyText];
    if (rgb==0 && dialogListCell(calls)) return colors[propertyText];
    if (rgb==0x333333 && startsWith(calls,{0x7347DF,0x73B4A7,0x712498,0x70FC6C,0x70FB75,
                                           0x50CAE,0x8B41AE}) && modalContent(calls,7))
        return colors[panelHeaderText];
    if (calls[0]==0x6C6570 && button(calls,1)) {
        if (rgb==0xDEBD8E) return colors[propertyMutedText];
        if (rgb==0x594304 || rgb==0x3D4E8F) return colors[propertyText];
    }
    if (startsWith(calls,{0x5BD040,0x5B9363,0x5B8E63,0x5B8C9E,0x38633}) ||
        startsWith(calls,{0xBE865,0xBEB6D,0xBDB85,0x38633,0xA82AF}) ||
        startsWith(calls,{0x389260,0x38A3B8,0x38633,0x3879E9})) {
        if (rgb==0x413003) return colors[propertyText];
        if (rgb==0x9F884E) return colors[propertyMutedText];
        if (calls[0]==0x389260 && rgb==0x9F6500) return colors[propertyGroupText];
    }
    return unchanged;
}
inline DWORD dialogFill(ULONG_PTR route,DWORD rgb,const Calls& calls,const ThemeProfile& profile) {
    const auto& colors=profile.colors;
    if (route==0x7900F) return dialogText(rgb,calls,profile);
    if (route==0x78794) {
        if (rgb==0xFCFAEB && startsWith(calls,{0x389260,0x38A3B8,0x38633,0x3879E9}))
            return colors[propertyBackground];
        if (rgb==0xFFFFFF && dialogListCell(calls)) return colors[propertyBackground];
    }
    if (route!=0xAE795) return unchanged;
    if (rgb==0xCCCCCC && startsWith(calls,{0x8BAAA7,0x8B9C41,0x8BB628,0x8B4365,
                                           0x8D5C70,0x8D3581,0x627774,0x638D8C}))
        return colors[panelHeaderBackground];
    if (rgb==0xB3AC86 && (modalContent(calls,0) || chapterDialog(calls,0))) return colors[panelBorder];
    if (rgb==0xFFFFFF) {
        if ((startsWith(calls,{0x303436,0x303167,0x811C25,0x811A26,0x8117C5,0x3666FF,
                               0x38633,0x261A67}) &&
             (modalContent(calls,8) || chapterDialog(calls,8) || dockList(calls,8))) ||
            (startsWith(calls,{0xA4638,0x8119D8,0x8117C5,0x3666FF,0x38633,0x261A67}) &&
             (modalContent(calls,6) || chapterDialog(calls,6) || dockList(calls,6))))
            return colors[propertyBackground];
    }
    if (rgb==0xFCFAEB &&
        (startsWith(calls,{0x86886A,0x892256,0x867B68,0x38633,0x401E88}) ||
         startsWith(calls,{0x5D33A5,0x5D1515,0x3E793C,0x3E6FC3}) ||
         startsWith(calls,{0x388B85,0x38A3B8,0x38633,0x3879E9}) ||
         startsWith(calls,{0xA4638,0x3891B4,0x38A3B8,0x38633,0x3879E9}) ||
         startsWith(calls,{0xA4638,0x5B981F,0x5B8D84,0x5B8C9E,0x38633}) ||
         startsWith(calls,{0xA4638,0xBEA0F,0xBDB85,0x38633,0xA82AF}) ||
         modalContent(calls,0)))
        return colors[propertyBackground];
    if (rgb==0xFFFFFF && startsWith(calls,{0x543C81,0x53AC15}) &&
        (mainDock(calls,2) || settingsDialog(calls,2))) return colors[propertyBackground];
    return unchanged;
}
inline DWORD fillColor(ULONG_PTR route,DWORD rgb,const Calls& calls,const ThemeProfile& profile) {
    const auto& colors=profile.colors;
    const auto zones=profile.zones;
    if ((zones&enableDockSplitter) && route==0xAE795 && rgb==0xF5F2DD &&
        startsWith(calls,{0xA4638,0x5A7B6B,0x5A883C,0x5A7033,0x38633,0x5A6852}) &&
        mainDock(calls,6)) return colors[dockSplitterFill];
    if (zones&enableTreeAndBars) {
        if (route==0xAE795 && rgb==0xFCFAEB &&
            startsWith(calls,{0x74EB1F,0x70FB4B,0x50CAE,0x50A15}) && mainDock(calls,4))
            return colors[barsBackground];
        if (route==0x7900F && rgb==0x594304 && helpToolbarText(calls)) return colors[barsText];
    }
    if (route==0x32F6 && (zones&enablePopupMenu)) {
        if (calls[1]==0xE433A && calls[2]==0xCCEAB && calls[3]==0xC57A2 && calls[4]==0xC5EEB) {
            if (rgb==0xEBE6CC && calls[0]==0xE44B1) return colors[popupMenuGutter];
            if (rgb==0xFCFAEB && calls[0]==0xE451C) return colors[popupMenuBackground];
        }
        if (rgb==0xD9D5C2 && startsWith(calls,{0xE4414,0xE2684,0xC6C39,0xCD0A7,0xCCF5D}))
            return mix(colors[popupMenuBackground],colors[popupMenuText],24);
    }
    if (route==0x32F6 && (zones&enableTreeAndProperties) && editChrome(calls,0x2007B2)) {
        if (rgb==0xFFFFFF) return colors[propertyBackground];
        if (rgb==0xFCFAEB) return mix(colors[propertyBackground],colors[panelBorder],22);
        if (rgb==0xB3AC86) return colors[panelBorder];
    }
    if (zones&enableTreeAndProperties) {
        const auto changed=dialogFill(route,rgb,calls,profile);
        if (changed!=unchanged) return changed;
    }
    if (route!=0xAE795) return unchanged;
    if ((zones&enablePopupMenu) && rgb==0x413003 &&
        startsWith(calls,{0x59AFA,0x5F676,0x5EE59,0x5F80C,0xE2B99})) return colors[popupMenuText];
    if ((zones&enableTreeAndBars) && rgb==0xF5F2DD && calls[0]==0x8BCF80 &&
        calls[1]==0x8BB628 && calls[2]==0x8B4365 && mainDock(calls,3)) return colors[barsBackground];
    if ((zones&enableTreeTabs) && (tabs(calls,0x5D567B,2) || dialogTabs(calls,0x5D567B,2))) {
        if (rgb==0xFCFAEB) return colors[treeTabsActive];
        if (rgb==0xEBE6CC) return colors[treeTabsInactive];
    }
    return unchanged;
}
inline DWORD glyphColor(DWORD rgb,const Calls& calls,const ThemeProfile& profile) {
    const auto& colors=profile.colors;
    if ((profile.zones&enableTreeAndBars) && rgb==0x594304 && helpToolbarText(calls))
        return colors[barsText];
    if ((profile.zones&enablePopupMenu) && calls[1]==0x5EE3E && calls[2]==0x5F80C &&
        calls[3]==0xE2B99 && calls[4]==0xE2665) {
        if (calls[0]==0x5F4AD) {
            if (rgb==0x413003) return colors[popupMenuText];
            if (rgb==0x9F884E) return mix(colors[popupMenuBackground],colors[popupMenuText],57);
        }
        if (calls[0]==0x5F5C2 && rgb==0xB38000) return colors[popupMenuShortcut];
    }
    if ((profile.zones&enableTreeAndProperties) && rgb==0xA7A7A7 &&
        calls[0]==0x20CEB9 && calls[1]==0x543CB0 && calls[2]==0x53AC15 && mainDock(calls,3))
        return colors[propertyMutedText];
    if (profile.zones&enableTreeAndProperties) {
        if (calls[0]==0x5441BA && calls[1]==0x53AC15 && (mainDock(calls,2) || settingsDialog(calls,2))) {
            if (rgb==0) return colors[propertyText];
            if (rgb==0x808080) return colors[propertyMutedText];
        }
        const auto changed=dialogText(rgb,calls,profile);
        if (changed!=unchanged) return changed;
    }
    if ((profile.zones&enableTreeTabs) && rgb==0x413003 &&
        (tabs(calls,0x5D48A2,1) || dialogTabs(calls,0x5D48A2,1)))
        return colors[treeTabsText];
    return unchanged;
}
inline DWORD solid(void* context) {
    auto* pattern=binding.getSource(context);
    double red{},green{},blue{},alpha{};
    if (!pattern || binding.rgba(pattern,&red,&green,&blue,&alpha) || alpha!=1) return unchanged;
    return (static_cast<DWORD>(std::lround(red*255))<<16) |
           (static_cast<DWORD>(std::lround(green*255))<<8) | static_cast<DWORD>(std::lround(blue*255));
}
inline void setColor(void* context,DWORD rgb) {
    binding.setRgb(context,((rgb>>16)&255)/255.,((rgb>>8)&255)/255.,(rgb&255)/255.);
}
inline bool themeableInk(DWORD rgb) {
    return rgb==0 || rgb==0x808080 || rgb==0xA7A7A7 || rgb==0x413003 || rgb==0x9F884E ||
           rgb==0xB38000 || rgb==0x594304 || rgb==0x3D4E8F || rgb==0xDEBD8E ||
           rgb==0x9F6500 || rgb==0x333333;
}
inline bool editorGutterPattern(void* context) {
    const auto* api=enterprise_gradient::detail::published.load(std::memory_order_acquire);
    auto* pattern=binding.getSource(context);
    void* surface{};
    if (!api || !pattern || api->type(pattern)!=1 || api->surface(pattern,&surface) || !surface)
        return false;
    const auto& image=enterprise_navigation::api.surface;
    return image.type(surface)==0 && image.width(surface)==8 && image.height(surface)==8;
}
inline bool editorGutter(void* context) {
    const auto calls=frameCalls();
    if (!startsWith(calls,{0x541921,0x543CBC,0x53AC15}) || !mainDock(calls,3)) return false;
    double x{},y{},right{},bottom{};
    binding.fillExtents(context,&x,&y,&right,&bottom);
    return right-x>0 && right-x<=64 && bottom-y>=32;
}
inline DWORD strokeColor(ULONG_PTR route,DWORD rgb,const Calls& calls,const ThemeProfile& profile) {
    if ((profile.zones&enableTreeAndProperties) && route==0xAE4EA &&
        ((rgb==0xCCC6AD && calls[0]==0x813D69) || (rgb==0xBFB999 && calls[0]==0x813BFF)) &&
        calls[1]==0x8104B0 && calls[2]==0x3028DA && calls[3]==0x80FE6A &&
        calls[4]==0x811A4D && calls[5]==0x8117C5)
        return profile.colors[panelBorder];
    if (!(profile.zones&enableTreeAndProperties) || route!=0x3659 || rgb!=0x333333 ||
        (calls[0]!=0x73BFEC && calls[0]!=0x73C028) ||
        calls[1]!=0x734528 || calls[2]!=0x73B930 || calls[3]!=0x712498 ||
        calls[4]!=0x70FC6C || calls[5]!=0x70FB75 || calls[6]!=0x50CAE ||
        calls[7]!=0x8B41AE || !modalContent(calls,8)) return unchanged;
    return profile.colors[panelHeaderText];
}
inline void stroke(void* context) {
    const auto route=reinterpret_cast<ULONG_PTR>(_ReturnAddress())-binding.graphics;
    const auto* profile=binding.profile->load(std::memory_order_acquire);
    const DWORD incoming=GetLastError();
    DWORD replacement=unchanged;
    if ((profile->zones&enableTreeAndProperties) && (route==0x3659 || route==0xAE4EA)) {
        const auto input=solid(context);
        if (input==0x333333 || input==0xCCC6AD || input==0xBFB999)
            replacement=strokeColor(route,input,frameCalls(),*profile);
        if (route==0xAE4EA && input==0xB3AC86)
            replacement=sheetHeaderPaint(route,input,sheetPaintCalls(),binding.frame,*profile);
    }
    if (replacement!=unchanged) { binding.save(context); setColor(context,replacement); }
    SetLastError(incoming);
    binding.stroke(context);
    const DWORD outgoing=GetLastError();
    if (replacement!=unchanged) binding.restore(context);
    SetLastError(outgoing);
}
inline void fill(void* context) {
    const auto route=reinterpret_cast<ULONG_PTR>(_ReturnAddress())-binding.graphics;
    const auto* profile=binding.profile->load(std::memory_order_acquire);
    const DWORD incoming=GetLastError();
    DWORD replacement=unchanged;
    if ((profile->zones&enableTreeAndProperties) && route==0xAAE3A) {
        const auto* api=enterprise_gradient::detail::published.load(std::memory_order_acquire);
        auto* pattern=binding.getSource(context);
        if (api && pattern && api->type(pattern)==4 && gridHeaderSurface(frameCalls()))
            replacement=mix(profile->colors[barsBackground],profile->colors[panelBorder],12);
    }
    if (profile->zones && (route==0x32F6 || route==0xAE795 || route==0x78794 || route==0x7900F)) {
        const auto input=solid(context);
        if ((profile->zones&enableTreeAndProperties) &&
            ((route==0xAE795 && sheetHeaderColor(input)) || (route==0x7900F && input==0x413003)))
            replacement=sheetHeaderPaint(route,input,sheetPaintCalls(),binding.frame,*profile);
        if (replacement==unchanged && (input==0xFFFFFF || input==0xFCFAEB || input==0xEBE6CC || input==0xF5F2DD ||
            input==0xD9D5C2 || input==0xCCCCCC || input==0xB3AC86 ||
            (route==0xAE795 && input==0x413003) || (route==0x7900F && themeableInk(input))))
            replacement=fillColor(route,input,frameCalls(),*profile);
        if (replacement==unchanged && route==0xAE795 && (profile->zones&enableTreeAndProperties) &&
            (input==0xFFFFFF || (input==unchanged && editorGutterPattern(context))) && editorGutter(context))
            replacement=mix(profile->colors[propertyBackground],profile->colors[panelBorder],18);
    }
    if (replacement!=unchanged) { binding.save(context); setColor(context,replacement); }
    SetLastError(incoming);
    binding.fill(context);
    const DWORD outgoing=GetLastError();
    if (replacement!=unchanged) binding.restore(context);
    SetLastError(outgoing);
}
inline void glyphs(void* context,const Glyph* glyph,int count) {
    const auto route=reinterpret_cast<ULONG_PTR>(_ReturnAddress())-binding.graphics;
    const auto* profile=binding.profile->load(std::memory_order_acquire);
    const DWORD incoming=GetLastError();
    DWORD replacement=unchanged;
    if ((profile->zones&(enableTreeAndProperties|enableTreeTabs|enablePopupMenu|enableTreeAndBars)) && route==0x78F95) {
        const auto input=solid(context);
        if (themeableInk(input))
            replacement=glyphColor(input,frameCalls(),*profile);
        if (replacement==unchanged && (profile->zones&enableTreeAndProperties) && input==0x413003)
            replacement=sheetHeaderPaint(route,input,sheetPaintCalls(),binding.frame,*profile);
    }
    if (replacement!=unchanged) { binding.save(context); setColor(context,replacement); }
    SetLastError(incoming);
    binding.show(context,glyph,count);
    const DWORD outgoing=GetLastError();
    if (replacement!=unchanged) binding.restore(context);
    SetLastError(outgoing);
}

inline void editGradient(double& red,double& green,double& blue) {
    if (red<.72 || green<.68 || blue<.48 || red-blue>.40) return;
    const auto& colors=binding.profile->load(std::memory_order_acquire)->colors;
    const auto color=mix(colors[propertyBackground],colors[panelBorder],red>.96 ? 14 : 32);
    red=((color>>16)&255)/255.; green=((color>>8)&255)/255.; blue=(color&255)/255.;
}
inline void keepColor(double&,double&,double&) {}
struct Skin { int width,height; const char* sha256; PaletteColor fill,detail; };
inline constexpr Skin skins[]={
    {12,12,"a69c9dd749a1312f917e731353bca54961cb401e3bf95e7294b8cf7dbfa3ef2c",propertyBackground,propertyText},
    {12,12,"0af37217b1d577cb252b46e89e54d0882d1858eecc810ad81230f1946297cc40",propertyBackground,propertyText},
    {12,12,"77d78aa116b7767735916d9c9cece5f45819c63c49af7737671d44b096570c38",propertyBackground,propertyText},
    {12,12,"8deaf0e06d326bb0551fd207517c26354e685a63034539c693589b06b571b33f",propertyBackground,propertyText},
    {12,12,"8c003209e8168be6500ff4e4eb08a84e363b5bc8c09182a8d7fd8bb56e3828cc",propertyBackground,propertyText},
    {12,12,"1fcb3add1e5cfd3a0b3c04abaf02125bd7028fc67b55029a9caf391e6adffce6",propertyBackground,propertyText},
    {12,12,"8e2afd35cf52621dce740ed4d42a52d376ba7bfb1a6224a6f76e1bc5e63bc45e",propertyBackground,propertyText},
    {12,12,"be6429498482b09a139f8f08129a598f26750135cfa271cee7dba9ddc735636a",propertyBackground,propertyText},
    {12,12,"1b8cb5cac2a51678c1dc14a430d0cc92101eaf83ade8de77e4049dceca2f2ae6",propertyBackground,propertyText},
    {12,12,"9fccce46f2113886e54344b5e4c99722d4fea8ef22367b1d682aa4b089a3eace",propertyBackground,propertyText},
    {12,12,"a86d4085edfe0186fe7dd6b226a3ec5bd9338697ee3d9d1fe1085d5ff6afcbb4",propertyBackground,propertyMutedText},
    {13,13,"ba4afffae673507cffcd486a728c069f0620fc906caa1c3d6a45e1dc6783c6ce",barsBackground,propertyMutedText},
    {13,13,"0b7762b672d82994c753083a6044f111fa9a24957763213c6893c517ca901413",barsBackground,propertyText},
    {13,13,"ba63c6f1cdefadb1324d53f69c1a8cde8fb8b68408656f9a4663d608ff56fa64",barsBackground,panelBorder},
    {13,13,"b170b792813df755d357e205414a39ed0e7595c5a6d036432d4ba1dcbbe17bad",barsBackground,propertyText},
    {13,13,"c4be8171feab809e8a989a4d2af99f689e62ea59164c379fd5f0b8569465de11",barsBackground,propertyMutedText},
    {7,7,"f5e3a9978a8e21661d9c0d899e2ddaa457b0f56124b255278e3babb991321971",treeTabsInactive,panelBorder},
    {7,7,"69dc3cd2d9d137f86be7e61621430fe4103bc0ddfd669c31bc92f0f886c55c39",treeTabsInactive,panelBorder},
    {7,7,"4f6411f661baf25849dd0c3f2cedc50826bd12177be6597f8b5c5ba35d8a0b7e",treeTabsActive,panelBorder},
    {7,7,"eeea2bb308678c96e8ee3e2f9823a5721559ebd60ddb3d6a350c5e2793993635",treeTabsActive,panelBorder},
    {7,7,"156494332a0b228db2175d04a3f5a9ce4c329036e32dce517e1a1534f99b75a3",treeTabsInactive,panelBorder},
    {7,7,"ab34a20d6cf858f23fa9d0757183276d7ae3716e4d6a607f3dec56fbd5321191",treeTabsInactive,panelBorder},
    {7,7,"8f70addce7c73d242ae43050d310b701a4695770c57ffaf7f0641b9123884632",treeTabsActive,panelBorder},
    {7,7,"275fa8b0e3ed38d5e2a874fb47de456bb28c9f82c53dd5f5d9660ee8c19b08d7",treeTabsActive,panelBorder},
    {16,70,"ffa58c0fc713bba69decccef0d3a67ceaf17d5fb68a6305c94a4c87f14bdf546",scrollerTrackBackground,scrollerArrowHover},
    {16,5,"f1c4b7a95db284260cf5f284a74fad2af8364e052b35ac11d0399c355108b63d",scrollerThumbBackground,scrollerThumbDetail},
    {16,5,"04370386e097d29574bb18790fd630c4b1fb6ec8270220003ddc41d0222ebeb5",scrollerThumbBackground,scrollerThumbDetail},
    {16,6,"2c992d566c5c465562f921e5c9b9bbda196ae9f4de1475313dceff8f9527c863",scrollerThumbHover,scrollerThumbDetail},
    {16,1,"ebe33a8aeb6e2ef32bbbebe9dddd2134950b0e86613be306979fd4411b8c3654",scrollerThumbBackground,scrollerThumbDetail},
    {16,70,"0a4ef7e62e9f00b0886134f4bfdcc089e9eea31a0efd62b511f7f9b91283c7ce",scrollerTrackBackground,scrollerThumbDetail},
    {16,21,"65eb563e30c0b46997fc4a442c58acae2bbd4ac2c8a8a2ac3f2ea52def6820e0",scrollerTrackBackground,scrollerArrowHover},
    {16,21,"451b419b2de3fbabbe7c9db839bed549c50b55b9e643dfe8965158869c8e1876",scrollerTrackBackground,scrollerArrowHover},
    // The Syntax Assistant tree uses the light track and arrow variants
    // even when the native list surface follows the current preset.
    {16,70,"0430f2891f8c4d2c3e3973e39a10fac355723693567d2bbca34d50d1969f5b06",scrollerTrackBackground,scrollerThumbDetail},
    {16,21,"f0a89668682bb5b0629f42baea2fcfb3191d96c8d5c5c8cd5408aa2704097a81",scrollerArrowBackground,scrollerArrowGlyph},
    {16,21,"e7b6553bf85bb45d9eb18f39180d00f19a19c5ebb96f64c1175c084108e22d09",scrollerArrowBackground,scrollerArrowGlyph},
    {16,5,"7f568c3ba534d2a763dcf75528c4e3c42778a7a0aabf65bf0fbd505e9b2494c5",scrollerThumbBackground,scrollerThumbDetail},
    {16,5,"74ddfc75fdaaefbd01986bf99527b2ce28ac1813a5e1f29f43b63323efaab7a9",scrollerThumbBackground,scrollerThumbDetail},
    {16,6,"06be4f2f02e02c757fbaa0a15451a03059317514e0c92f8dd5a33273bda9718d",scrollerThumbHover,scrollerThumbDetail},
    {16,1,"26c5fc748cd280990621a6ac733c1a461e12c319c455f5e0e6d10fe901a529af",scrollerThumbBackground,scrollerThumbDetail},
    {70,16,"2008b1b31371db51d9b0a00f53eeb3f0b19e2f6866deece2f8edae84eac40522",scrollerTrackBackground,scrollerThumbDetail},
    {21,16,"11856e37d44420b3d1118d6f2c9cfee63aac109abc2764cbca5e9ecc273e2f2f",scrollerArrowBackground,scrollerArrowGlyph},
    {21,16,"587779cd6fbe9acd7683dac125591db90155dbbc0ae0b2f51c1cc47e3c92bade",scrollerArrowBackground,scrollerArrowGlyph},
    {5,16,"2cad2cdeb9bd4c01a87434a4f3a3d974ac0c7177ed56acab4750864d5024e097",scrollerThumbBackground,scrollerThumbDetail},
    {5,16,"5cea666cf8cb54358f14128094627c5fb346c1e269eebc73790c3efafab98881",scrollerThumbBackground,scrollerThumbDetail},
    {5,16,"f11d9acf50346fb6f8d6a2e1f3ceb492f09f87b098632fbda94eac87891945f4",scrollerThumbHover,scrollerThumbDetail},
    {1,16,"2a69366e4decc7215ee258820d384410b20fb39fd96b8a76041da7aceea0f534",scrollerThumbBackground,scrollerThumbDetail},
};
inline bool scrollbarSkinSize(int width,int height) {
    return (width==16 && height!=16) || (height==16 && width!=16);
}
inline void* skinUnsafe(void* surface) {
    const auto& api=enterprise_navigation::api;
    thumb_png::detail::Image info{};
    if (!thumb_png::detail::describe(api.surface,surface,info)) return nullptr;
    bool eligible{};
    for (const auto& asset:skins) eligible|=asset.width==info.width && asset.height==info.height;
    if (!eligible) return nullptr;
    const auto* profile=binding.profile->load(std::memory_order_acquire);
    const auto zones=profile->zones;
    if ((info.width==7 && !(zones&enableTreeTabs)) ||
        ((info.width==12 || info.width==13) && !(zones&enableTreeAndProperties)) ||
        (scrollbarSkinSize(info.width,info.height) && !(zones&enableScrollerPng))) return nullptr;
    api.surface.flush(surface);
    auto* pixels=api.surface.data(surface);
    if (!scroller_png::detail::pixelRange(pixels,info.bytes,false)) return nullptr;
    BYTE canonical[16*70*4]{};
    const auto width=static_cast<size_t>(info.width)*4;
    for (int row=0;row<info.height;++row) {
        auto* to=canonical+row*width;
        std::memcpy(to,pixels+row*static_cast<size_t>(info.stride),width);
        if (info.format==thumb_png::detail::rgb24)
            for (size_t alpha=3;alpha<width;alpha+=4) to[alpha]=255;
    }
    BYTE digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,canonical,static_cast<ULONG>(width*info.height),digest,sizeof digest)) return nullptr;
    constexpr char digits[]="0123456789abcdef";
    char hex[65]{};
    for (size_t i=0;i<std::size(digest);++i) {hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}
    for (const auto& asset:skins)
        if (asset.width==info.width && asset.height==info.height && !std::strcmp(asset.sha256,hex))
            return thumb_png::clone(api,surface,{profile->colors[asset.fill],profile->colors[asset.detail],
                                                  asset.width==13});
    return nullptr;
}
inline void* skin(void* surface) {
    __try { return skinUnsafe(surface); }
    __except (thumb_png::detail::exceptionFilter(GetExceptionCode())) { return nullptr; }
}
inline void* controlTile(void* surface,ULONG_PTR zones) {
    const auto& api=enterprise_navigation::api;
    thumb_png::detail::Image tile{};
    if (!thumb_png::detail::describe(api.surface,surface,tile)) return nullptr;
    const bool buttonSize=(zones&enableTreeAndProperties) &&
        (tile.width==5 || (tile.width>=30 && tile.width<=128)) &&
        (tile.height==5 || tile.height==10);
    const bool tabSize=(zones&enableTreeTabs) &&
        (tile.width==8 || tile.width==44) && (tile.height==2 || tile.height==8);
    if (!buttonSize && !tabSize) return nullptr;
    const auto calls=frameCalls();
    const auto& colors=binding.profile->load(std::memory_order_acquire)->colors;
    if (tabSize && calls[1]==0x5D5608 && calls[2]==0x5D4842 &&
        (calls[3]==0x5D3ED9 || calls[3]==0x5D3F15) && calls[4]==0x5D347E &&
        calls[5]==0x5D1515 && calls[6]==0x3E793C && calls[7]==0x3E6FC3) {
        const auto role=calls[3]==0x5D3F15 ? treeTabsActive : treeTabsInactive;
        return thumb_png::clone(api,surface,{colors[role],colors[panelBorder]});
    }
    if (!buttonSize || calls[1]!=0x6C5B30 || !button(calls,2)) return nullptr;
    switch (calls[0]) {
    case 0x421AB6: case 0x421B89: case 0x421C65: case 0x421D36: case 0x421E23:
    case 0x421F10: case 0x422008: case 0x4220FD: case 0x4221E7:
        break;
    default: return nullptr;
    }
    const auto face=calls[3]==0x2AB37 ? mix(colors[barsBackground],colors[panelBorder],40) :
                                      colors[barsBackground];
    return thumb_png::clone(api,surface,{face,colors[panelBorder]});
}
inline void* chromeImage(void* surface) {
    if (auto* replacement=skin(surface)) return replacement;
    const auto zones=binding.profile->load(std::memory_order_acquire)->zones;
    if (!(zones&(enableTreeAndProperties|enableTreeTabs))) return nullptr;
    __try { return controlTile(surface,zones); }
    __except (thumb_png::detail::exceptionFilter(GetExceptionCode())) { return nullptr; }
}
inline void setSource(void* context,void* pattern) {
    const auto route=reinterpret_cast<ULONG_PTR>(_ReturnAddress())-binding.graphics;
    const auto zones=binding.profile->load(std::memory_order_acquire)->zones;
    if (zones && route==0x13724) {
        enterprise_gradient::setSource(binding.source,context,pattern,&keepColor,nullptr,&chromeImage);
        return;
    }
    if ((zones&enableTreeAndProperties) && route==0x4C42) {
        const DWORD incoming=GetLastError();
        const bool edit=editChrome(frameCalls(),0x2009D9);
        SetLastError(incoming);
        if (edit) { enterprise_gradient::setSource(binding.source,context,pattern,&editGradient); return; }
    }
    binding.source(context,pattern);
}
} // namespace detail

inline DWORD ensure(ULONG_PTR frame,ULONG_PTR graphics,const std::atomic<const ThemeProfile*>& profile) {
    using namespace detail;
    if (installed) return ERROR_SUCCESS;
    const auto cairo=GetModuleHandleW(L"cairo.dll");
    if (!cairo) return ERROR_MOD_NOT_FOUND;
    if (!enterprise_gradient::initialize(cairo) || !enterprise_navigation::initialize(cairo))
        return ERROR_NOT_SUPPORTED;
    binding.frame=frame; binding.graphics=graphics; binding.profile=&profile;
#define DESIGNER_CAIRO(member,name) \
    binding.member=reinterpret_cast<decltype(binding.member)>(GetProcAddress(cairo,name)); \
    if (!binding.member) return ERROR_PROC_NOT_FOUND
    DESIGNER_CAIRO(fill,"cairo_fill");
    DESIGNER_CAIRO(stroke,"cairo_stroke");
    DESIGNER_CAIRO(show,"cairo_show_glyphs");
    DESIGNER_CAIRO(source,"cairo_set_source");
    DESIGNER_CAIRO(save,"cairo_save");
    DESIGNER_CAIRO(restore,"cairo_restore");
    DESIGNER_CAIRO(getSource,"cairo_get_source");
    DESIGNER_CAIRO(rgba,"cairo_pattern_get_rgba");
    DESIGNER_CAIRO(setRgb,"cairo_set_source_rgb");
    DESIGNER_CAIRO(fillExtents,"cairo_fill_extents");
#undef DESIGNER_CAIRO
    struct Hook { ULONG_PTR rva; PVOID oldValue,newValue; };
    const Hook hooks[]={
        {0xC7A90,reinterpret_cast<PVOID>(binding.stroke),reinterpret_cast<PVOID>(&stroke)},
        {0xC7AE8,reinterpret_cast<PVOID>(binding.fill),reinterpret_cast<PVOID>(&fill)},
        {0xC7B08,reinterpret_cast<PVOID>(binding.source),reinterpret_cast<PVOID>(&setSource)},
        {0xC7B90,reinterpret_cast<PVOID>(binding.show),reinterpret_cast<PVOID>(&glyphs)}
    };
    for (const auto& hook:hooks)
        if (!scroller_png_hook::imageRegion(graphics+hook.rva,graphics,sizeof(PVOID),PAGE_READONLY) ||
            *reinterpret_cast<PVOID volatile*>(graphics+hook.rva)!=hook.oldValue) return ERROR_INVALID_DATA;
    auto* first=reinterpret_cast<PVOID*>(graphics+hooks[0].rva);
    const auto bytes=hooks[std::size(hooks)-1].rva-hooks[0].rva+sizeof(PVOID);
    DWORD protect{};
    if (!VirtualProtect(first,bytes,PAGE_READWRITE,&protect)) return GetLastError();
    size_t applied{};
    for (const auto& hook:hooks) {
        if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(graphics+hook.rva),
                hook.newValue,hook.oldValue)!=hook.oldValue) break;
        ++applied;
    }
    DWORD status=applied==std::size(hooks) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    DWORD ignored{};
    if (status || !VirtualProtect(first,bytes,protect,&ignored)) {
        if (!status) status=GetLastError();
        while (applied) {
            const auto& hook=hooks[--applied];
            if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(graphics+hook.rva),
                    hook.oldValue,hook.newValue)!=hook.newValue) status=themeStateUncertain;
        }
        if (!VirtualProtect(first,bytes,protect,&ignored)) status=themeStateUncertain;
    }
    installed=status==ERROR_SUCCESS;
    return status;
}
} // namespace designer_chrome
