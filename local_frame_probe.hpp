#pragma once

#include "platform.hpp"

inline constexpr DWORD frameProbeVersion = 1;
inline constexpr size_t frameProbeCapacity = 64;
inline constexpr char frameProbeExport[] = "RunLocalFrameProbeV1";
inline constexpr char frameProbeAbiExport[] = "FrameProbeAbiV1";

enum class FrameSite : DWORD { Background, Child };
struct FrameSiteDefinition {
    DWORD rva;
    std::array<BYTE, 16> signature;
};
inline constexpr std::array<FrameSiteDefinition, 2> frameSites{{
    {0x71A6D3, {0x49, 0x8B, 0xD6, 0x48, 0x8B, 0xCD, 0xFF, 0x15,
                0x89, 0x9E, 0x30, 0x00, 0x48, 0x8B, 0x4C, 0x24}},
    {0x7124C2, {0x48, 0x8B, 0xD6, 0x41, 0xFF, 0xD2, 0x90, 0x48,
                0x8B, 0x4D, 0xB0, 0x48, 0x85, 0xC9, 0x74, 0x0F}}
}};

enum class FrameLocationState : DWORD { NotRead, Null, ReadFailed, Unmapped, Image };
struct FrameLocation {
    ULONG_PTR imageBase{};
    DWORD rva{};
    FrameLocationState state{};
    bool operator==(const FrameLocation&) const = default;
};

struct FrameProbeRect {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const FrameProbeRect&) const = default;
};

// The key records observed numeric attributes, without object or stack addresses.
// It is never used as a lifetime identity or to link background and child events.
struct FrameIdentity {
    FrameLocation owner, rawItem, drawInterface, drawMethod;
    FrameProbeRect rect;
    FrameSite site{};
    DWORD colorOrFlag{};
    bool operator==(const FrameIdentity&) const = default;
};

struct FrameProbeSample {
    FrameIdentity identity;
    ULONGLONG calls{};
};

struct FrameProbeCounts {
    ULONGLONG accepted{};
    ULONGLONG mismatches{};
    ULONGLONG readFailures{};
    ULONGLONG printing{};
    ULONGLONG unknownIdentities{};
    ULONGLONG capacityDrops{};
    ULONGLONG busyDrops{};
};

struct FramePatchReport {
    DWORD originalProtection{};
    DWORD finalProtection{};
    DWORD cleanupError{};
};

struct FrameProbeTrace {
    DWORD size{sizeof(FrameProbeTrace)};
    DWORD version{frameProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD patchError{};
    DWORD snapshotError{};
    std::array<FramePatchReport, frameSites.size()> patches{};
    std::array<FrameProbeCounts, frameSites.size()> counts{};
    std::array<FrameProbeSample, frameProbeCapacity> samples{};
};

static_assert(sizeof(FrameLocation) == 16);
static_assert(sizeof(FrameProbeRect) == 16);
static_assert(sizeof(FrameProbeSample) == 96);
static_assert(sizeof(FrameProbeTrace) == 6304);
