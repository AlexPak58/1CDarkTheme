#pragma once

#include "property_description_identity.hpp"

inline constexpr DWORD propertyProbeVersion = 2;
inline constexpr size_t propertyProbeCapacity = 128;
inline constexpr DWORD propertyProbeCaptureMs = 60000;
inline constexpr DWORD propertyProbeWaitMs = propertyProbeCaptureMs + 30000;
inline constexpr DWORD propertySiteRva = 0x5D33CB;
inline constexpr std::array<BYTE, 16> propertySiteSignature{
    0x48, 0x8B, 0xD5, 0x41, 0xFF, 0x93, 0xE8, 0x01,
    0x00, 0x00, 0x48, 0x8B, 0x0D, 0xC4, 0x58, 0x7F
};
inline constexpr char propertyProbeExport[] = "RunPropertyDescriptionProbeV2";
inline constexpr char propertyProbeAbiExport[] = "PropertyDescriptionProbeAbiV2";

struct PropertyRect {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const PropertyRect&) const = default;
};

inline bool validPropertyRect(const PropertyRect& rect)
{
    const auto width = static_cast<LONGLONG>(rect.right) - rect.left;
    const auto height = static_cast<LONGLONG>(rect.bottom) - rect.top;
    return width > 0 && width <= MAXLONG && height > 0 && height <= MAXLONG;
}

struct PropertySample {
    propertyDescription::Identity identity;
    PropertyRect rect;
    ULONGLONG calls{};
};

struct PropertyProbeCounts {
    ULONGLONG description{}, list{}, foreignId10{}, mismatches{}, readFailures{}, capacityDrops{}, busyDrops{};
    std::array<ULONGLONG, static_cast<size_t>(propertyDescription::Stage::Count)> rejectedStages{};
};

struct PropertyProbePatch { DWORD originalProtection{}, finalProtection{}, cleanupError{}; };

struct PropertyProbeTrace {
    DWORD size{sizeof(PropertyProbeTrace)};
    DWORD version{propertyProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD patchError{};
    DWORD snapshotError{};
    PropertyProbePatch patch;
    PropertyProbeCounts counts;
    std::array<PropertySample, propertyProbeCapacity> samples{};
};

static_assert(sizeof(PropertySample) == 48);
static_assert(sizeof(PropertyProbeTrace) == 6368);
