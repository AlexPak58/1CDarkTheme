#pragma once

#include "platform.hpp"

struct TooltipTraceReturn {
    ULONG_PTR stackOffset;
    ULONG_PTR rva;
    ULONG_PTR alternateRva{};
};

struct TooltipTracePlatform {
    const wchar_t* executableHash;
    const wchar_t* rendererHash;
    const wchar_t* graphicsHash;
    ULONG_PTR rgbaSlot;
    ULONG_PTR rgbaReturn;
    std::array<TooltipTraceReturn, 6> graphicsReturns;
    TooltipTraceReturn rendererReturn;
};

inline constexpr TooltipTracePlatform tooltipTracePlatforms[] = {
    {exeHash, frameHash, graphicsHash, 0xC7C20, 0x2336,
        {{{0x40, 0x1366E}, {0x2A0, 0x4911}, {0x470, 0x9277},
          {0x500, 0x25BB6}, {0x6A0, 0x5977}, {0x810, 0x9D45}}},
        {0x940, 0x5FC7F5}},
    {L"636AB6F2379B197BC85CE3F7C46DE35438778A78FEFB37522B939F6974F551C5",
        L"BFEBDD44BA710CD07897C15155A5DA9AEB84847AE9420734D7A0E287DE8EBD1F",
        L"6759A6589ED088D24ADDFE4C33467570E9AF08D4939725592DEC8831D4C0D0A3",
        0xE1C28, 0x29F7,
        {{{0x40, 0x147A0}, {0x2A0, 0x51EB, 0x52C9}, {0x470, 0x9D18},
          {0x500, 0x27309}, {0x6E0, 0x63C3}, {0x8D0, 0xA7A5}}},
        {0xA00, 0x748C06}},
};

inline const TooltipTracePlatform* findTooltipTracePlatform(const std::wstring& executable)
{
    for (const auto& platform : tooltipTracePlatforms) {
        if (matchesSha256(executable, platform.executableHash)) return &platform;
    }
    return nullptr;
}

struct TooltipTraceColor {
    DWORD rgba{};
    DWORD calls{};
};

struct TooltipTrace {
    std::array<TooltipTraceColor, 16> colors{};
    DWORD otherCalls{};
};

static_assert(sizeof(TooltipTrace) == 132);
