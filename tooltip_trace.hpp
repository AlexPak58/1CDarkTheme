#pragma once

#include "platform.hpp"

struct TooltipTraceReturn {
    ULONG_PTR stackOffset;
    ULONG_PTR rva;
    ULONG_PTR alternateRva{};
};

struct TooltipTraceGraphics {
    ULONG_PTR rgbaSlot;
    ULONG_PTR rgbaReturn;
    std::array<TooltipTraceReturn, 6> returns;
};

struct TooltipTracePlatform {
    const wchar_t* executableHash;
    const wchar_t* rendererHash;
    const wchar_t* graphicsHash;
    const TooltipTraceGraphics& graphics;
    TooltipTraceReturn rendererReturn;
};

inline constexpr TooltipTraceGraphics tooltipGraphics8325{
    0xC6B38, 0x2336,
    {{{0x40, 0x1357E}, {0x270, 0x4875}, {0x400, 0x91D7},
      {0x490, 0x256F6}, {0x630, 0x58D7}, {0x7A0, 0x9CA5}}},
};
inline constexpr TooltipTraceGraphics tooltipGraphics8327{
    0xC7C20, 0x2336,
    {{{0x40, 0x1366E}, {0x2A0, 0x4911}, {0x470, 0x9277},
      {0x500, 0x25BB6}, {0x6A0, 0x5977}, {0x810, 0x9D45}}},
};
inline constexpr TooltipTraceGraphics tooltipGraphics8511302And1343{
    0xE0C28, 0x2447,
    {{{0x40, 0x14670}, {0x2A0, 0x4C3B, 0x4D19}, {0x470, 0x9768},
      {0x500, 0x27209}, {0x6E0, 0x5E13}, {0x8D0, 0xA1F5}}},
};
inline constexpr TooltipTraceGraphics tooltipGraphics8511529{
    0xE1C28, 0x29F7,
    {{{0x40, 0x147A0}, {0x2A0, 0x51EB, 0x52C9}, {0x470, 0x9D18},
      {0x500, 0x27309}, {0x6E0, 0x63C3}, {0x8D0, 0xA7A5}}},
};

inline constexpr TooltipTracePlatform tooltipTracePlatforms[] = {
    {exeHash, frameHash, graphicsHash, tooltipGraphics8327, {0x940, 0x5FC7F5}},
    {L"90A1632B6B32D649189A4B53F51444B06CA41F58301D80D648948C52148F6D9E",
        L"246FB2907AC37A7F5267D7E09A78BB8BD4B6EA94C849996C6FFCA6C267411C8C",
        L"205173892ADEC7568E4320EC713F201A85513FFFDC01B05CF0513E4185F0A101",
        tooltipGraphics8325, {0x8D0, 0x5F9CA5}},
    {L"664C3695B12B55CE362CF7B127C551F57E4770D5DF29D022DBF3ECA91752C6CE",
        L"EAA8B2AFD541D6C111E0896178A5E413D70A4AC0122CDB6363C93FCBF6A58950",
        L"03DAE8F2A0E5A39322FC69B9B259FAC9A41EEC229254DC94B9C41EE7AC01FCB9",
        tooltipGraphics8327, {0x940, 0x5FC7C5}},
    {L"BCE653192BE5D31D583DD698F78579D21D555413BCF208E9881F03B3ADCEB556",
        L"E592556AD68F13B143FE445587191E685ADDDBAD00A9FC7276A4086945A766F9",
        L"C2738571BF193776E5A4227FAD46F410E43595A1AEC0160BBE30A5564966344C",
        tooltipGraphics8327, {0x940, 0x5FC7C5}},
    {L"4A611D73B2570FC86FF6308A3743AA0F8BF9039855773525FD2D7EAE32D83F5A",
        L"9C3A6D26BBA96633E951D13F377F37A3689372AFF10143BC8778FB6C78A8A9F6",
        L"092DDB20C492EB308D1AA2859329FDF61FE2FEB94ECCA30E0BBF629BF4A3BE00",
        tooltipGraphics8511302And1343, {0xA00, 0x747F76}},
    {L"091A89F6DA9EE9B04CD65399866CA1A51F1376A331605BDF36C628646A2FEA7B",
        L"6834E4A4652ED29FBF64AD2164DA1EE6FF2FAEB4C6118B33AA517EFFF2F46486",
        L"FC51D01E471987383672DBB1EC9F4DCFB70CF7872A5B71A9AC7F3E74D182817C",
        tooltipGraphics8511302And1343, {0xA00, 0x748616}},
    {L"636AB6F2379B197BC85CE3F7C46DE35438778A78FEFB37522B939F6974F551C5",
        L"BFEBDD44BA710CD07897C15155A5DA9AEB84847AE9420734D7A0E287DE8EBD1F",
        L"6759A6589ED088D24ADDFE4C33467570E9AF08D4939725592DEC8831D4C0D0A3",
        tooltipGraphics8511529, {0xA00, 0x748C06}},
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
