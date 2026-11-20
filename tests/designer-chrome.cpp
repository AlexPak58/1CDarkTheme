#include "designer_chrome.hpp"
#include <cstdio>

int main() {
    using namespace designer_chrome::detail;
    ThemeProfile profile;
    profile.zones=enableTreeAndProperties|enableTreeAndBars|enableTreeTabs|enablePopupMenu;
    profile.colors[propertyBackground]=0x2B323B;
    profile.colors[treeTabsActive]=0x536681;
    profile.colors[treeTabsInactive]=0x303A46;
    profile.colors[popupMenuBackground]=0x282C32;
    profile.colors[popupMenuGutter]=0x343B46;
    profile.colors[popupMenuText]=0xC5C7C9;
    profile.colors[popupMenuShortcut]=0xACB3BB;
    const Calls edit{0x2007B2,0x38633,0x21322D,0x8D5C70,0x8D3581};
    const Calls selectedTab{0x5D567B,0x5D4842,0x5D3F15,0x5D347E,0x5CE319};
    const Calls gripper{0x8BCF80,0x8BB628,0x8B4365,0x8D5C70,0x8D3581};
    const Calls name{0x5441BA,0x53AC15,0x8D5C70,0x8D3581,0};
    const Calls hint{0x20CEB9,0x543CB0,0x53AC15,0x8D5C70,0x8D3581};
    const Calls menuBody{0xE451C,0xE433A,0xCCEAB,0xC57A2,0xC5EEB};
    const Calls menuGutter{0xE44B1,0xE433A,0xCCEAB,0xC57A2,0xC5EEB};
    const Calls menuDivider{0xE4414,0xE2684,0xC6C39,0xCD0A7,0xCCF5D};
    const Calls submenuArrow{0x59AFA,0x5F676,0x5EE59,0x5F80C,0xE2B99};
    const Calls plainEditor{0x543C81,0x53AC15,0x8D5C70,0x8D3581};
    const Calls dialogSurface{0x86886A,0x892256,0x867B68,0x38633,0x401E88};
    const Calls helpToolbar{0x74EB1F,0x70FB4B,0x50CAE,0x50A15,0x8D5C70,0x8D3581};
    const Calls helpLabel{0x7347DF,0x739469,0x712498,0x70FC6C,0x74EBA4,0x70FB75,
                          0x50CAE,0x50A15,0x8D5C70,0x8D3581};
    const Calls dialogClose{0x73BFEC,0x734528,0x73B930,0x712498,0x70FC6C,0x70FB75,
                             0x50CAE,0x8B41AE,0x8D5C70,0x8D3581,0x627774,0x638D8C,0x7B0D70};
    const Calls label{0x5BD040,0x5B9363,0x5B8E63,0x5B8C9E,0x38633};
    const Calls dialogCell{0x85F445,0x812B2B,0x303E26,0x303167,0x811C25,0x811A26,
                           0x8117C5,0x3666FF,0x38633,0x261A67,0x8D5C70,0x8D3581,
                           0x627774,0x638D8C,0x7B0D70};
    auto unrelatedCell=dialogCell;
    unrelatedCell[12]=0;
    const Calls chapterCell{0x85F445,0x812B2B,0x303E26,0x303167,0x811C25,0x811A26,
                            0x8117C5,0x3666FF,0x38633,0x261A67,0x8D5C70,0x8D3581,
                            0x628382,0x639C5B,0x638BBB,0x946F72};
    const Calls chapterEmpty{0xA4638,0x8119D8,0x8117C5,0x3666FF,0x38633,0x261A67,
                             0x8D5C70,0x8D3581,0x628382,0x639C5B,0x638BBB,0x946F72};
    const Calls dockedContents{0x303436,0x303167,0x811C25,0x811A26,0x8117C5,0x3666FF,
                              0x38633,0x261A67,0x8D5C70,0x8D3581};
    auto otherModalCell=chapterCell;
    otherModalCell[14]=0x638BBC;
    auto queryModalCell=chapterCell;
    queryModalCell[15]=0x7F068A;
    auto queryModalEmpty=chapterEmpty;
    queryModalEmpty[11]=0x7F068A;
    const Calls gridLabel{0x811725,0x810A94,0x3028DA,0x80FE6A,0x811A4D,0x8117C5,
                          0x3666FF,0x38633,0x261A67};
    auto wrappedGridLabel=gridLabel;
    wrappedGridLabel[0]=0x8116C4;
    const Calls gridFace{0x8139C3,0x8104B0,0x3028DA,0x80FE6A,0x811A4D,0x8117C5,
                         0x3666FF,0x38633,0x261A67};
    auto gridBorder=gridFace;
    gridBorder[0]=0x813BFF;
    constexpr ULONG_PTR sheetFrame=0x180000000;
    const Calls sheetHeading{0x105028,0x10F809,0x1B547A,0xE5772,0xD3F3D,
                             sheetFrame+0x8D5C70,sheetFrame+0x8D3581};
    const Calls sheetHeadingText{0x105704,0x10F809,0x1B5AE9,0x1B577E,0xE5772,0xD3F3D,
                                 sheetFrame+0x8D5C70,sheetFrame+0x8D3581};
    const Calls menuLabel{0x5F4AD,0x5EE3E,0x5F80C,0xE2B99,0xE2665};
    const Calls menuShortcut{0x5F5C2,0x5EE3E,0x5F80C,0xE2B99,0xE2665};
    auto foreign=edit;
    foreign[4]=0;
    const bool ok=
        fillColor(0x32F6,0xFFFFFF,edit,profile)==profile.colors[propertyBackground] &&
        fillColor(0x32F6,0xFCFAEB,edit,profile)==mix(profile.colors[propertyBackground],profile.colors[panelBorder],22) &&
        fillColor(0x32F6,0xFFFFFF,foreign,profile)==unchanged &&
        fillColor(0x32F6,0xCC6699,edit,profile)==unchanged &&
        fillColor(0xAE795,0xFCFAEB,selectedTab,profile)==profile.colors[treeTabsActive] &&
        fillColor(0xAE795,0xEBE6CC,selectedTab,profile)==profile.colors[treeTabsInactive] &&
        fillColor(0xAE795,0xF5F2DD,gripper,profile)==profile.colors[barsBackground] &&
        glyphColor(0,name,profile)==profile.colors[propertyText] &&
        glyphColor(0xA7A7A7,hint,profile)==profile.colors[propertyMutedText] &&
        glyphColor(0xFFFFFF,name,profile)==unchanged &&
        fillColor(0x32F6,0xFCFAEB,menuBody,profile)==profile.colors[popupMenuBackground] &&
        fillColor(0x32F6,0xEBE6CC,menuGutter,profile)==profile.colors[popupMenuGutter] &&
        fillColor(0x32F6,0xD9D5C2,menuDivider,profile)!=unchanged &&
        fillColor(0xAE795,0x413003,submenuArrow,profile)==profile.colors[popupMenuText] &&
        fillColor(0xAE795,0x413003,foreign,profile)==unchanged &&
        glyphColor(0x413003,menuLabel,profile)==profile.colors[popupMenuText] &&
        glyphColor(0x9F884E,menuLabel,profile)==mix(profile.colors[popupMenuBackground],profile.colors[popupMenuText],57) &&
        glyphColor(0xB38000,menuShortcut,profile)==profile.colors[popupMenuShortcut] &&
        glyphColor(0x3D4E8F,menuLabel,profile)==unchanged &&
        fillColor(0xAE795,0xFFFFFF,plainEditor,profile)==profile.colors[propertyBackground] &&
        fillColor(0xAE795,0x242930,plainEditor,profile)==unchanged &&
        fillColor(0xAE795,0xFCFAEB,dialogSurface,profile)==profile.colors[propertyBackground] &&
        fillColor(0xAE795,0xFCFAEB,helpToolbar,profile)==profile.colors[barsBackground] &&
        glyphColor(0x594304,helpLabel,profile)==profile.colors[barsText] &&
        strokeColor(0x3659,0x333333,dialogClose,profile)==profile.colors[panelHeaderText] &&
        strokeColor(0x3659,0x222222,dialogClose,profile)==unchanged &&
        strokeColor(0x3659,0x333333,unrelatedCell,profile)==unchanged &&
        glyphColor(0x413003,label,profile)==profile.colors[propertyText] &&
        fillColor(0x78794,0xFFFFFF,dialogCell,profile)==profile.colors[propertyBackground] &&
        fillColor(0x78794,0x536AC2,dialogCell,profile)==unchanged &&
        glyphColor(0,dialogCell,profile)==profile.colors[propertyText] &&
        glyphColor(0,chapterCell,profile)==profile.colors[propertyText] &&
        fillColor(0xAE795,0xFFFFFF,chapterEmpty,profile)==profile.colors[propertyBackground] &&
        glyphColor(0,otherModalCell,profile)==unchanged &&
        glyphColor(0,queryModalCell,profile)==profile.colors[propertyText] &&
        fillColor(0xAE795,0xFFFFFF,queryModalEmpty,profile)==profile.colors[propertyBackground] &&
        glyphColor(0x594304,gridLabel,profile)==profile.colors[propertyText] &&
        glyphColor(0x594304,wrappedGridLabel,profile)==profile.colors[propertyText] &&
        gridHeaderSurface(gridFace) && !gridHeaderSurface(gridBorder) &&
        strokeColor(0xAE4EA,0xBFB999,gridBorder,profile)==profile.colors[panelBorder] &&
        strokeColor(0xAE4EA,0x7F8B96,gridBorder,profile)==unchanged &&
        sheetHeaderPaint(0xAE795,0xF7F5E6,sheetHeading,sheetFrame,profile)!=unchanged &&
        sheetHeaderPaint(0xAE795,0xFFFFFF,sheetHeading,sheetFrame,profile)==unchanged &&
        sheetHeaderPaint(0x78F95,0x413003,sheetHeadingText,sheetFrame,profile)==profile.colors[propertyText] &&
        sheetHeaderPaint(0x78F95,0x413003,sheetHeadingText,sheetFrame+1,profile)==unchanged &&
        fillColor(0xAE795,0xFFFFFF,dockedContents,profile)==profile.colors[propertyBackground] &&
        glyphColor(0,unrelatedCell,profile)==unchanged &&
        glyphColor(0x413003,hint,profile)==unchanged;
    profile.zones=0;
    if (!ok || fillColor(0x32F6,0xFFFFFF,edit,profile)!=unchanged ||
        glyphColor(0,name,profile)!=unchanged ||
        fillColor(0x32F6,0xFCFAEB,menuBody,profile)!=unchanged) {
        std::puts("FAIL Designer chrome selectors");
        return 1;
    }
    std::puts("PASS Designer chrome selectors, stock palette and unrelated controls");
    return 0;
}
