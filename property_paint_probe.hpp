#pragma once

#include "property_paint_identity.hpp"

inline constexpr DWORD propertyPaintProbeVersion = 1;
inline constexpr size_t propertyPaintProbeCapacity = 128;
inline constexpr DWORD propertyPaintProbeCaptureMs = 60000;
inline constexpr DWORD propertyPaintProbeWaitMs = propertyPaintProbeCaptureMs + 30000;
inline constexpr DWORD propertyPaintSiteRva = 0x3E7960;
inline constexpr std::array<BYTE, 16> propertyPaintSiteSignature{
    0x48, 0x8D, 0x8F, 0x60, 0x01, 0x00, 0x00, 0xE8,
    0xB4, 0x98, 0x1E, 0x00, 0x33, 0xF6, 0x48, 0x89
};
inline constexpr char propertyPaintProbeExport[] = "RunPropertyPaintProbeV1";
inline constexpr char propertyPaintProbeAbiExport[] = "PropertyPaintProbeAbiV1";

struct PropertyPaintArguments {
    DWORD layout{}, condition{}, skipFill{}, brushPresent{};
    bool operator==(const PropertyPaintArguments&) const = default;
};

inline bool validPropertyPaintArguments(const PropertyPaintArguments& args)
{
    return args.skipFill <= 0xFF && args.brushPresent <= 1;
}

struct PropertyPaintSample {
    propertyDescription::Identity identity;
    PropertyPaintArguments arguments;
    ULONGLONG calls{};
};

struct PropertyPaintProbeCounts {
    ULONGLONG description{}, list{}, foreignId10{}, mismatches{}, readFailures{}, capacityDrops{}, busyDrops{};
    std::array<ULONGLONG, static_cast<size_t>(propertyDescription::Stage::Count)> rejectedStages{};
};

struct PropertyPaintProbePatch { DWORD originalProtection{}, finalProtection{}, cleanupError{}; };

struct PropertyPaintProbeTrace {
    DWORD size{sizeof(PropertyPaintProbeTrace)};
    DWORD version{propertyPaintProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD patchError{};
    DWORD snapshotError{};
    PropertyPaintProbePatch patch;
    PropertyPaintProbeCounts counts;
    std::array<PropertyPaintSample, propertyPaintProbeCapacity> samples{};
};

static_assert(sizeof(PropertyPaintSample) == 48);
static_assert(sizeof(PropertyPaintProbeTrace) == 6368);
