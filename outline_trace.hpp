#pragma once

#include "form_line_trace.hpp"

struct OutlineTracePlatform {
    const FormLineTracePlatform& client;
    const wchar_t* rendererHash;
    ULONG_PTR rgbSlot;
    size_t rendererStackOffset;
    ULONG_PTR rgbaReturn;
    std::array<ULONG_PTR, 4> graphicsReturns;
    std::array<ULONG_PTR, 2> rendererReturns;
};

// Reuse the client hashes, grphcs hashes and RGBA slots from the form-line probe.
inline constexpr OutlineTracePlatform outlineTracePlatforms[] = {
    // 8.3.24.1819
    {formLineTracePlatforms[0],
     L"9F35AFABF061639150135B8811F90C1014C06B4FAD99B4649948BB8CB46B2E70",
     0xC6B20, 0x1A0, 0xAE1B9, {0xADC5D, 0xAE302, 0xA964F, 0xB2557}, {0xC1CFB, 0xC1D85}},
    // 8.3.25.1445
    {formLineTracePlatforms[1],
     L"41F76A6402B2840F144759285082BB28E50FE83D187A40EC759A5B6EEBA536CE",
     0xC6B20, 0x1A0, 0xAE1C9, {0xADC6D, 0xAE312, 0xA965F, 0xB25A7}, {0xC1E56, 0xC1EDA}},
    // 8.3.27.1989
    {formLineTracePlatforms[2],
     L"2B29F70CD4B70561F324D08433ECFEAC9E47A814F246E6A9A4B401B8F204F407",
     0xC7B60, 0x1A0, 0xAEA39, {0xAE4BD, 0xAEB82, 0xA9E5F, 0xB2E17}, {0xC24C6, 0xC254A}},
    // 8.3.27.2130
    {formLineTracePlatforms[3],
     L"6FBD6E3238D5FFCE7A61E9F6C52917F1D65BDD616763C2E265C512986C13EDCD",
     0xC7B60, 0x1A0, 0xAEA59, {0xAE4DD, 0xAEBA2, 0xA9E7F, 0xB2E37}, {0xC1BB6, 0xC1C3A}},
    // 8.3.27.2325
    {formLineTracePlatforms[4],
     L"E73A71D156199E425E09B32FAD3DFA64905F1D59F66564279707632573914B37",
     0xC7B60, 0x1A0, 0xAF329, {0xAEDAD, 0xAF472, 0xAA74F, 0xB3707}, {0xC1BB6, 0xC1C3A}},
    // 8.5.1.1302
    {formLineTracePlatforms[5],
     L"8CA6D3001BA0F30880772D853EC751EA7BD84ACE505A31009502437F83FF46A0",
     0xE0C10, 0x1B0, 0xC8FA9, {0xC8A2D, 0xC90F2, 0xC3E7F, 0xCD16C}, {0xDE676, 0xDE6FA}},
    // 8.5.1.1343
    {formLineTracePlatforms[6],
     L"E0DE4695C526485A60A077CB3626D0B229CC46A0F5D9450E52AC833C3DFFA347",
     0xE0C10, 0x1B0, 0xC8FC9, {0xC8A4D, 0xC9112, 0xC3E9F, 0xCD18C}, {0xDE676, 0xDE6FA}},
    // 8.5.1.1529
    {formLineTracePlatforms[7],
     L"B76143305D0E0865E1227DA6C935677B538545CA0BBDA692A96555EC473A4C14",
     0xE1C10, 0x1B0, 0xC9959, {0xC93DD, 0xC9AA2, 0xC482F, 0xCDB1C}, {0xDE686, 0xDE70A}},
};

inline const OutlineTracePlatform* findOutlineTracePlatform(const std::wstring& executable,
                                                           bool designer = false)
{
    const auto* client = findFormLineTracePlatform(executable, designer);
    if (!client) return nullptr;
    for (const auto& platform : outlineTracePlatforms) {
        if (&platform.client == client) return &platform;
    }
    return nullptr;
}

struct OutlineTraceSample {
    RECT rect{};
    DWORD inputColor{};
    DWORD rgba{};
    DWORD rendererReturn{};
    BOOL mask{};
    DWORD calls{};
};

enum class TraceOutcome { Caller, Bounds, Graphics, Renderer, InvalidSample, Match, Count };
using TraceStageCounts = std::array<ULONGLONG, static_cast<size_t>(TraceOutcome::Count)>;

struct OutlineTrace {
    std::array<OutlineTraceSample, 16> outlines{};
    DWORD otherCalls{};
    std::array<TraceStageCounts, 2> stages{}; // RGB, RGBA
};

static_assert(sizeof(OutlineTrace) == 680);
