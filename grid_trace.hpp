#pragma once

#include "outline_trace.hpp"

struct GridTracePlatform {
    ULONG_PTR rectangleSlot;
    ULONG_PTR rectangleReturn;
    ULONG_PTR dispatchReturn;
    ULONG_PTR fillReturn;
    ULONG_PTR solidReturn;
    size_t rendererStackOffset;
    std::array<ULONG_PTR, 2> rendererReturns; // horizontal, vertical
    ULONG_PTR wrapperReturn{};
    size_t wrapperFrameSize{};
};

// Same order and binary identities as outlineTracePlatforms.
inline constexpr GridTracePlatform gridTracePlatforms[] = {
    {0xC6BC8, 0xAB5B3, 0xB3052, 0xAB5CC, 0xADD06, 0xB0, {0xC5DBD, 0xC5E0B}}, // 8.3.24.1819
    {0xC6BC0, 0xAB5C3, 0xB30A2, 0xAB5DC, 0xADD16, 0xB0, {0xC6140, 0xC618E}}, // 8.3.25.1445
    {0xC7A68, 0xABDD3, 0xB3912, 0xABDEC, 0xAE566, 0xB0, {0xC67B0, 0xC67FE}}, // 8.3.27.1989
    {0xC7A68, 0xABDF3, 0xB3932, 0xABE0C, 0xAE586, 0xB0, {0xC5EA0, 0xC5EEE}}, // 8.3.27.2130
    {0xC7A68, 0xAC6C3, 0xB4202, 0xAC6DC, 0xAEE56, 0xB0, {0xC5EA0, 0xC5EEE}, 0xC164A, 0x50}, // 8.3.27.2325
    {0xE0B08, 0xC60F3, 0xCDDC7, 0xC610C, 0xC8AD6, 0xC0, {0xE3850, 0xE389E}}, // 8.5.1.1302
    {0xE0B08, 0xC6113, 0xCDDE7, 0xC612C, 0xC8AF6, 0xC0, {0xE3850, 0xE389E}}, // 8.5.1.1343
    {0xE1B08, 0xC6AA3, 0xCE777, 0xC6ABC, 0xC9486, 0xC0, {0xE3860, 0xE38AE}}, // 8.5.1.1529
};
static_assert(std::size(gridTracePlatforms) == std::size(outlineTracePlatforms));

struct GridRectangleSample {
    std::array<double, 4> rectangle{}; // x, y, width, height
    DWORD rendererReturn{};
    DWORD calls{};
};

struct GridColorSample {
    DWORD rgba{};
    DWORD rendererReturn{};
    BOOL mask{};
    DWORD calls{};
};

struct GridRendererSample {
    DWORD stackOffset{};
    DWORD rva{};
    DWORD calls{};
    DWORD onePixel{};
    DWORD moxelCaller{}; // parent return RVA when it falls in moxel.dll code
};

struct GridTrace {
    std::array<GridRectangleSample, 16> rectangles{};
    std::array<GridColorSample, 16> colors{};
    DWORD otherRectangles{};
    DWORD otherColors{};
    std::array<TraceStageCounts, 3> stages{}; // RGB, RGBA, rectangle
    std::array<GridRendererSample, 16> rendererCandidates{};
    DWORD noRenderer{};
    DWORD otherRenderers{};
};
static_assert(sizeof(GridTrace) == 1376);
