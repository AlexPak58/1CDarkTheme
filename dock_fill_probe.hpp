#pragma once

#include "platform.hpp"

inline constexpr DWORD dockProbeVersion = 1;
inline constexpr size_t dockProbeCapacity = 128;
inline constexpr char dockProbeExport[] = "RunDockFillProbeV1";
inline constexpr char dockProbeAbiExport[] = "DockFillProbeAbiV1";

enum class DockSite : DWORD { Basic, WithEdge };
struct DockSiteDefinition {
    DWORD rva, drawMethodRva;
    std::array<BYTE, 16> signature;
};
inline constexpr std::array<DockSiteDefinition, 2> dockSites{{
    {0x92AF88, 0x92AF30, {0x48, 0x8B, 0xD5, 0x48, 0x89, 0xB4, 0x24, 0xA0,
                        0x00, 0x00, 0x00, 0x48, 0x8B, 0x35, 0x06, 0xDD}},
    {0x93214B, 0x9320E0, {0x48, 0x8B, 0xCF, 0x48, 0x8B, 0x07, 0xFF, 0x90,
                        0x30, 0x02, 0x00, 0x00, 0x8B, 0x55, 0xC8, 0x48}}
}};

struct DockOwnerDefinition { DockSite site; DWORD rootRva, sideRva, drawRva; };
inline constexpr std::array<DockOwnerDefinition, 3> dockOwners{{
    {DockSite::Basic, 0xB2FC08, 0xB2F0B8, 0xB2EFF8},
    {DockSite::Basic, 0xB31DB8, 0xB31D88, 0xB32180},
    {DockSite::WithEdge, 0xB2F780, 0xB2F750, 0xB2FB48}
}};

inline const DockOwnerDefinition* dockOwner(DockSite site, ULONG_PTR rootRva)
{
    for (const auto& owner : dockOwners)
        if (owner.site == site && owner.rootRva == rootRva) return &owner;
    return nullptr;
}

enum class DockLocationState : DWORD { NotRead, Null, Unmapped, Image };
struct DockLocation {
    ULONG_PTR imageBase{};
    DWORD rva{};
    DockLocationState state{};
    bool operator==(const DockLocation&) const = default;
};

struct DockProbeRect {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const DockProbeRect&) const = default;
};

inline bool isClippedDockRect(const DockProbeRect& full, const DockProbeRect& clipped)
{
    return clipped.left < clipped.right && clipped.top < clipped.bottom &&
        clipped.left >= full.left && clipped.top >= full.top &&
        clipped.right <= full.right && clipped.bottom <= full.bottom;
}

// Object addresses and renderer arguments such as the brush and VDC stay in the handler.
struct DockIdentity {
    DockLocation caller;
    DockProbeRect full, clipped;
    DockSite site{};
    DWORD rootTypeRva{}, rawColor{}, orientationRaw{};
    bool operator==(const DockIdentity&) const = default;
};

struct DockProbeSample { DockIdentity identity; ULONGLONG calls{}; };
struct DockProbeCounts {
    ULONGLONG accepted{}, mismatches{}, readFailures{}, unknownIdentities{}, capacityDrops{}, busyDrops{};
};
struct DockPatchReport { DWORD originalProtection{}, finalProtection{}, cleanupError{}; };
struct DockProbeTrace {
    DWORD size{sizeof(DockProbeTrace)};
    DWORD version{dockProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD patchError{};
    DWORD snapshotError{};
    std::array<DockPatchReport, dockSites.size()> patches{};
    std::array<DockProbeCounts, dockSites.size()> counts{};
    std::array<DockProbeSample, dockProbeCapacity> samples{};
};

static_assert(sizeof(DockLocation) == 16);
static_assert(sizeof(DockProbeRect) == 16);
static_assert(sizeof(DockProbeSample) == 72);
static_assert(sizeof(DockProbeTrace) == 9360);
