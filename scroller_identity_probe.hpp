#pragma once

#include "platform.hpp"

inline constexpr DWORD scrollerProbeVersion = 1;
inline constexpr size_t scrollerProbeCapacity = 128;
inline constexpr char scrollerProbeExport[] = "RunScrollerIdentityProbeV1";
inline constexpr char scrollerProbeAbiExport[] = "ScrollerIdentityProbeAbiV1";
inline constexpr auto scrollerWbaseHash =
    L"8BC18B9428A1FCFABD77FB1D30D660860434B9F8F1586DFF0387E396F7DA53DE";

enum class ScrollerSite : DWORD { Dispatch, Component };
struct ScrollerSiteDefinition {
    DWORD rva;
    std::array<BYTE, 16> signature;
};
inline constexpr std::array<ScrollerSiteDefinition, 2> scrollerSites{{
    {0x1D6A2, {0x49, 0x8B, 0xD6, 0x48, 0x8B, 0xCE, 0xFF, 0x90,
               0xC0, 0x00, 0x00, 0x00, 0x0F, 0x28, 0x44, 0x24}},
    {0x1E9D7, {0x48, 0x8B, 0xD5, 0x48, 0x8B, 0x01, 0xFF, 0x50,
               0x10, 0x48, 0x83, 0xC7, 0x10, 0x48, 0x83, 0xC3}}
}};

inline constexpr DWORD scrollerEntryRva = 0x1D600;
inline constexpr DWORD scrollerCommonDrawRva = 0x1E860;
struct ScrollerTableDefinition { DWORD tableRva, drawRva; };
inline constexpr std::array<ScrollerTableDefinition, 15> scrollerTables{{
    {0x3C590, scrollerCommonDrawRva}, {0x3C660, scrollerCommonDrawRva},
    {0x3C730, scrollerCommonDrawRva}, {0x3C800, scrollerCommonDrawRva},
    {0x3C8D0, scrollerCommonDrawRva}, {0x3C9A0, 0x1EBC0},
    {0x3CA78, scrollerCommonDrawRva}, {0x3CB50, scrollerCommonDrawRva},
    {0x3CC28, scrollerCommonDrawRva}, {0x3CD00, 0x1EFE0},
    {0x3CDD8, scrollerCommonDrawRva}, {0x3CEB0, scrollerCommonDrawRva},
    {0x3CFE8, scrollerCommonDrawRva}, {0x3D0B8, scrollerCommonDrawRva},
    {0x3D1F8, scrollerCommonDrawRva}
}};

inline DWORD scrollerDrawRva(ULONG_PTR tableRva)
{
    for (const auto& entry : scrollerTables)
        if (entry.tableRva == tableRva) return entry.drawRva;
    return 0;
}

enum class ScrollerLocationState : DWORD { NotRead, Null, ReadFailed, Unmapped, Image };
struct ScrollerLocation {
    ULONG_PTR imageBase{};
    DWORD rva{};
    ScrollerLocationState state{};
    bool operator==(const ScrollerLocation&) const = default;
};

struct ScrollerProbeRect {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const ScrollerProbeRect&) const = default;
};

// A numeric aggregation key, not an object lifetime ID or a UI surface label.
struct ScrollerIdentity {
    ScrollerLocation scroller, window, caller, component, componentMethod;
    ScrollerProbeRect rect;
    ScrollerSite site{};
    DWORD dispatchMethodRva{};
    DWORD componentOffset{};
    DWORD drawFlag{};
    bool operator==(const ScrollerIdentity&) const = default;
};

struct ScrollerProbeSample { ScrollerIdentity identity; ULONGLONG calls{}; };
struct ScrollerProbeCounts {
    ULONGLONG accepted{}, mismatches{}, readFailures{}, unknownIdentities{}, capacityDrops{}, busyDrops{};
};
struct ScrollerPatchReport { DWORD originalProtection{}, finalProtection{}, cleanupError{}; };
struct ScrollerProbeTrace {
    DWORD size{sizeof(ScrollerProbeTrace)};
    DWORD version{scrollerProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD patchError{};
    DWORD snapshotError{};
    std::array<ScrollerPatchReport, scrollerSites.size()> patches{};
    std::array<ScrollerProbeCounts, scrollerSites.size()> counts{};
    std::array<ScrollerProbeSample, scrollerProbeCapacity> samples{};
};

static_assert(sizeof(ScrollerLocation) == 16);
static_assert(sizeof(ScrollerProbeRect) == 16);
static_assert(sizeof(ScrollerProbeSample) == 120);
static_assert(sizeof(ScrollerProbeTrace) == 15504);

inline bool unresolvedScrollerIdentity(const ScrollerIdentity& identity)
{
    return identity.window.state != ScrollerLocationState::Image ||
        identity.caller.state != ScrollerLocationState::Image ||
        (identity.site == ScrollerSite::Component &&
            (identity.component.state != ScrollerLocationState::Image ||
             identity.componentMethod.state != ScrollerLocationState::Image));
}
