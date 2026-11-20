#include "../enterprise.cpp"
#include <cstdio>
static double observedAlpha, observedR, observedG, observedB;
static void* observedPattern;
void captureSource(void*,void* pattern) { observedPattern=pattern; }
void captureRgb(void*, double r, double g, double b) { observedR=r;observedG=g;observedB=b; }
void capture(void*, double r, double g, double b, double alpha) {
    captureRgb(nullptr,r,g,b);observedAlpha=alpha;
}
double luminance(double r,double g,double b) {
    const auto linear=[](double v) {return v<=.04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4);};
    return .2126*linear(r)+.7152*linear(g)+.0722*linear(b);
}

struct Color { double r,g,b; };
static int assertions{},failures{};
void check(bool valid,const char* message) {
    ++assertions;
    if (!valid) {++failures;std::fprintf(stderr,"FAIL: %s\n",message);}
}
double hue(Color c) {
    const auto hi=(std::max)(c.r,(std::max)(c.g,c.b)),lo=(std::min)(c.r,(std::min)(c.g,c.b));
    const auto d=hi-lo;
    if (d<1e-12) return 0;
    auto value=hi==c.r ? (c.g-c.b)/d : hi==c.g ? 2+(c.b-c.r)/d : 4+(c.r-c.g)/d;
    value*=60;
    return value<0 ? value+360 : value;
}
bool sameHue(Color a,Color b) {
    const auto difference=std::fabs(hue(a)-hue(b));
    return (std::min)(difference,360-difference)<1e-5;
}
double lum(Color c) {return luminance(c.r,c.g,c.b);}
double contrast(Color a,Color b) {
    const auto x=lum(a),y=lum(b);
    return ((std::max)(x,y)+.05)/((std::min)(x,y)+.05);
}
Color mappedSurface(Color c) {surface(c.r,c.g,c.b);return c;}
int main() {
    palette=ThemeProfile{};
    const Color foreground{229/255.0,235/255.0,245/255.0},workspace{31/255.0,41/255.0,56/255.0};
    Color c{1,1,1};vectorColor(c.r,c.g,c.b);
    check(c.r==workspace.r&&c.g==workspace.g&&c.b==workspace.b,"white surface uses workspace");
    c={0,0,0};text(c.r,c.g,c.b);
    check(c.r==foreground.r&&c.g==foreground.g&&c.b==foreground.b,"neutral black text uses readable foreground");

    // Every representative fill must retain its source hue and brightness
    // distinctions. The old palette collapsed blue/yellow and two red shades.
    const Color fills[]={
        {.98,.82,.82}, {.90,.70,.70}, // same red hue, distinct brightness/saturation
        {1,.84,.65}, {1,.89,.63}, {.80,1,.80},
        {.76,.97,1}, {.75,.85,.95}, {.93,.80,.98},
        {.98,.96,.96}, // weak semantic tint, previously mistaken for neutral
        {1,.8,.2}, {1,0,0}, {0,.5,0}, {0,0,1}
    };
    Color mapped[sizeof fills/sizeof fills[0]]{};
    for (size_t i=0;i<sizeof fills/sizeof fills[0];++i) {
        const auto source=fills[i];mapped[i]=mappedSurface(source);
        check(channels(mapped[i].r,mapped[i].g,mapped[i].b),"mapped semantic fill channels valid");
        check(sameHue(source,mapped[i]),"semantic fill hue retained");
        check(std::fabs(lum(mapped[i])-lum(source)*.12)<1e-6,"semantic fill retains scaled relative brightness");
        check(contrast(foreground,mapped[i])>=4.5,"default text readable on semantic fill");
        // Common scaling preserves RGB ratios as well as hue; no palette-blue
        // offset or normalization may erase weaker tint/saturation differences.
        const auto scale=source.r>0 ? mapped[i].r/source.r : source.g>0 ? mapped[i].g/source.g : mapped[i].b/source.b;
        check(std::fabs(mapped[i].r-source.r*scale)<1e-6&&std::fabs(mapped[i].g-source.g*scale)<1e-6&&
              std::fabs(mapped[i].b-source.b*scale)<1e-6,"semantic fill channel ratios retained");
        auto gradient=source;gradientColor(gradient.r,gradient.g,gradient.b);
        check(sameHue(source,gradient)&&std::fabs(lum(gradient)-lum(source)*.12)<1e-6,
              "gradient fill uses the same semantic hue and brightness contract");
    }
    check(lum(mapped[0])>lum(mapped[1])+1e-3,"two red shades keep original brightness ordering");
    check(std::fabs(mapped[0].r-mapped[1].r)+std::fabs(mapped[0].g-mapped[1].g)+std::fabs(mapped[0].b-mapped[1].b)>.01,
          "two same-hue red shades remain visibly distinct");
    for (size_t i=0;i<8;++i) for (size_t j=i+1;j<8;++j)
        check(std::fabs(mapped[i].r-mapped[j].r)+std::fabs(mapped[i].g-mapped[j].g)+std::fabs(mapped[i].b-mapped[j].b)>.01,
              "red orange yellow green cyan blue purple do not collapse into one palette color");
    c={.5,.5,.5};surface(c.r,c.g,c.b);
    check(c.r==.5&&c.g==.5&&c.b==.5,"dark neutral fill retained");
    c={.85,.85,.85};surface(c.r,c.g,c.b);
    check(c.r==workspace.r&&c.g==workspace.g&&c.b==workspace.b,"light neutral surface maps to workspace");
    const auto header=mappedSurface({1,.949,.631});
    const auto search=mappedSurface({1,.98,.854});
    check(header.b>header.g && header.g>header.r && lum(header)<lum(search) &&
          lum(search)<lum(workspace),"stock yellow chrome and search are darker than white work areas");
    const auto stripe=mappedSurface({250/255.,250/255.,250/255.});
    const auto listHeader=mappedSurface({242/255.,242/255.,242/255.});
    check(lum(workspace)<lum(stripe) && lum(stripe)<lum(listHeader),"list retains alternating bands and header");
    const auto selected=mappedSurface({254/255.,242/255.,199/255.});
    const auto focused=mappedSurface({253/255.,230/255.,143/255.});
    check(selected.b>selected.g && selected.g>selected.r && lum(focused)>lum(selected),
          "selection and focused cell keep distinct blue shades");
    const Color selectionSource{83/255.,106/255.,194/255.};
    auto textSelection=selectionSource;
    check(editSelection(textSelection.r,textSelection.g,textSelection.b,1) &&
          textSelection.b>textSelection.g && contrast(foreground,textSelection)>=4.79,
          "observed Taxi edit selection has a subdued blue background with readable text");
    auto translucentSelection=selectionSource;
    check(!editSelection(translucentSelection.r,translucentSelection.g,translucentSelection.b,.6) &&
          translucentSelection.r==selectionSource.r,"translucent blue fill is not an edit selection");
    const auto defaultProfile=palette;
    palette.colors[workspaceBackground]=0xFFD0ED;
    palette.colors[propertyBackground]=0xFFF0F9;
    palette.colors[propertySelection]=0xFF68C4;
    palette.colors[propertyText]=0x30051F;
    const auto pinkCanvas=mappedSurface({1,1,1});
    Color pinkTotal{1,1,1};
    surface(pinkTotal.r,pinkTotal.g,pinkTotal.b,true);
    check(lum(pinkTotal)>lum(pinkCanvas) && contrast(pinkCanvas,pinkTotal)<1.07,
          "light totals and labels have a subtle fill close to the pink canvas");
    auto pinkSelection=selectionSource;
    check(editSelection(pinkSelection.r,pinkSelection.g,pinkSelection.b,1) &&
          pinkSelection.r>pinkSelection.b && contrast({1,1,1},pinkSelection)>=4.79,
          "pink edit selection preserves the accent with readable selected text");
    palette.colors[propertyMutedText]=0x65133F;
    Color pinkHint{.74,.74,.74};
    text(pinkHint.r,pinkHint.g,pinkHint.b,true);
    check(contrast(pinkHint,pinkCanvas)>=4.5,"pale placeholder remains readable in a light preset");
    palette=defaultProfile;
    active.store(false);
    check(switchRetro() && themeEnabled() && currentPalette().colors[workspaceBackground] == 0xF2EFE7,
          "retro activates its own light palette from native");
    const auto retroFill=mappedSurface({.9,.2,.2});
    check(retroFill.r>retroFill.g && retroFill.g>.65,"retro keeps conditional fill hue with light tint");
    const auto retroPaper=mappedSurface({1,1,1});
    const auto retroPanel=mappedSurface({242/255.,242/255.,242/255.});
    Color retroHint{.83,.84,.85};
    text(retroHint.r,retroHint.g,retroHint.b,true);
    check(lum(retroPaper)>lum(retroPanel) && contrast(retroHint,retroPanel)>=4.5 &&
          contrast(retroHint,retroFill)>=4.5,"retro separates paper from chrome and keeps muted labels readable");
    Color retroSelectedText{1,1,1};
    text(retroSelectedText.r,retroSelectedText.g,retroSelectedText.b,true);
    auto retroSelection=selectionSource;
    check(editSelection(retroSelection.r,retroSelection.g,retroSelection.b,1) &&
          contrast(retroSelectedText,retroSelection)>=4.79,"retro keeps white selected text readable");
    Color retroHighlight{.98,.98,.98}, retroFace{.92,.92,.92}, retroShadow{.78,.78,.78};
    gradientColor(retroHighlight.r,retroHighlight.g,retroHighlight.b);
    gradientColor(retroFace.r,retroFace.g,retroFace.b);
    gradientColor(retroShadow.r,retroShadow.g,retroShadow.b);
    check(lum(retroHighlight)>lum(retroFace)+.14 && lum(retroFace)>lum(retroShadow)+.15,
          "retro controls keep three distinct bevel tones");
    check(!switchRetro() && !themeEnabled() && currentPalette().colors==palette.colors,
          "second retro action restores previous native state");
    active.store(true);
    check(switchRetro() && !switchRetro() && themeEnabled() && currentPalette().colors==palette.colors,
          "retro restores the configured theme when it was enabled");
    auto selectedProfile = palette;
    selectedProfile.colors[workspaceBackground] = 0x222A36;
    check(switchRetro() && applyLiveProfile(selectedProfile) && themeEnabled() &&
          !retroMode.load() && currentPalette().colors[workspaceBackground] == 0x222A36,
          "live choice exits the temporary retro palette immediately");
    switchTheme();
    switchTheme();
    check(themeEnabled() && currentPalette().colors[workspaceBackground] == 0x222A36,
          "live palette remains selected after switching off and on");
    auto malformedProfile = selectedProfile;
    malformedProfile.colors[workspaceBackground] = 0x1000000;
    check(!applyLiveProfile(malformedProfile) && currentPalette().colors[workspaceBackground] == 0x222A36,
          "invalid live palette leaves the current state intact");
    check(applyLiveProfile(ThemeProfile{}),"restore original preset for remaining checks");

    const Color statuses[]={{1,0,0},{.7,.35,.05},{.7,.65,0},{0,.5,0},{0,.45,.5},{.1,.2,.65},{.5,.1,.6},{0,1,1}};
    for (const auto source:statuses) {
        c=source;text(c.r,c.g,c.b,true);
        check(sameHue(source,c),"colored status glyph hue retained");
        const bool red=source.r>=.55 && source.r>=source.g+.35 && source.r>=source.b+.25;
        const auto worstBackground=(std::max)(red ? .045 : .12,lum(workspace));
        const auto sourceContrast=(lum(source)+.05)/(worstBackground+.05);
        check((lum(c)+.05)/(worstBackground+.05)>=4.5-1e-6,"status glyph readable on brightest mapped fill");
        if (sourceContrast>=4.5)
            check(c.r==source.r&&c.g==source.g&&c.b==source.b,"already readable status glyph unchanged");
        else {
            check(lum(c)>lum(source),"low contrast status glyph lightened");
            // Minimal white blend: it stops at the contrast threshold instead
            // of applying one fixed amount to every semantic glyph.
            check((lum(c)+.05)/(worstBackground+.05)<=4.5001,"status glyph uses minimum contrast correction");
        }
        auto vector=source;vectorColor(vector.r,vector.g,vector.b);
        check(sameHue(source,vector)&&(lum(vector)+.05)/(worstBackground+.05)>=4.5-1e-6,
              "unknown vector status path preserves hue and readable glyph contrast");
    }
    c={.85,.85,.85};outline(c.r,c.g,c.b);
    check(contrast(c,workspace)>=3,"neutral table outline distinct from workspace");
    check((lum(c)+.05)/(.12+.05)>=3-1e-6,"neutral grid visible on maximum mapped fill luminance");
    check(contrast(c,mapped[0])>=3-1e-6&&contrast(c,mapped[4])>=3-1e-6,
          "neutral grid visible over red and green conditional rows");
    for (const auto source:statuses) {
        auto stroke=source;outline(stroke.r,stroke.g,stroke.b);
        check(sameHue(source,stroke),"chromatic grid stroke keeps its semantic hue");
        check((lum(stroke)+.05)/(.12+.05)>=3-1e-6,"chromatic grid visible on maximum mapped fill luminance");
        check(contrast(stroke,mapped[0])>=3-1e-6&&contrast(stroke,mapped[4])>=3-1e-6,
              "chromatic grid visible over red and green conditional rows");
        check((source.r<=source.g)==(stroke.r<=stroke.g)&&
              (source.g<=source.b)==(stroke.g<=stroke.b)&&
              (source.r<=source.b)==(stroke.r<=stroke.b),"chromatic stroke channel order retained");
        if (lum(source)>=.46)
            check(source.r==stroke.r&&source.g==stroke.g&&source.b==stroke.b,"already readable chromatic stroke unchanged");
    }
    c={1,1,1};gradientColor(c.r,c.g,c.b);
    check(c.r==workspace.r&&c.g==workspace.g&&c.b==workspace.b,"neutral gradient maps to workspace");
    c={1,0,0};gradientColor(c.r,c.g,c.b);
    check(sameHue({1,0,0},c)&&std::fabs(lum(c)-lum({1,0,0})*.12)<1e-6,
          "chromatic gradient retains hue and relative brightness");

    originalRgba=reinterpret_cast<PVOID>(&capture);originalRgb=reinterpret_cast<PVOID>(&captureRgb);active.store(true);
    hookedRgba(nullptr,1,1,1,.375);
    check(observedAlpha==.375,"native source alpha unchanged");
    htmlReady.store(false);htmlRgb(nullptr,1,1,1);
    check(observedR==1&&observedG==1&&observedB==1,"disabled HTML hook passes color through");
    htmlReady.store(true);htmlRgb(nullptr,1,1,1);
    check(observedR==workspace.r&&observedG==workspace.g&&observedB==workspace.b,"HTML neutral surface uses workspace");
    htmlRgba(nullptr,0,0,0,.375);
    check(observedR==foreground.r&&observedG==foreground.g&&observedB==foreground.b&&observedAlpha==.375,"HTML neutral text and alpha preserved");
    originalSource=&captureSource;htmlReady.store(false);htmlSource(nullptr,reinterpret_cast<void*>(0x1234));
    check(observedPattern==reinterpret_cast<void*>(0x1234),"disabled HTML source passes pattern through");
    active.store(false);htmlReady.store(true);htmlSource(nullptr,reinterpret_cast<void*>(0x5678));
    check(observedPattern==reinterpret_cast<void*>(0x5678),"inactive theme passes HTML pattern through");
    check(SetThemeProfile(nullptr)==ERROR_INVALID_PARAMETER,"null profile rejected");
    ThemeProfile invalid;invalid.version=0;
    check(SetThemeProfile(&invalid)==ERROR_INVALID_PARAMETER,"invalid profile version rejected");
    configured=true;ThemeProfile valid;
    check(SetThemeProfile(&valid)==ERROR_ALREADY_ASSIGNED,"configured DLL rejects profile replacement");
    std::printf("%s: semantic hue/brightness, fill/glyph contrast and hook contracts: %d assertions, %d failures\n",failures?"FAIL":"PASS",assertions,failures);
    return failures?1:0;
}
