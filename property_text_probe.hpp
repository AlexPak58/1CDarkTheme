#pragma once
#include "property_text_identity.hpp"
inline constexpr DWORD propertyTextProbeVersion = 2;
inline constexpr size_t propertyTextProbeCapacity = 128;
inline constexpr DWORD propertyTextProbeCaptureMs = 60000;
inline constexpr DWORD propertyTextProbeWaitMs = propertyTextProbeCaptureMs + 30000;
inline constexpr DWORD propertyTextSiteRva = 0x327A0;
inline constexpr std::array<BYTE, 16> propertyTextSiteSignature{
    0x48,0x8B,0xCE,0xFF,0x50,0x78,0x90,0x48,0x89,0x1F,0x48,0x8B,0x4D,0x3F,0x48,0x33
};
inline constexpr char propertyTextProbeExport[] = "RunPropertyTextProbeV2";
inline constexpr char propertyTextProbeAbiExport[] = "PropertyTextProbeAbiV2";
inline constexpr auto propertyTextModuleHash = L"061C851224A6AB3782BDC43D24D44BD5D008672DF4F39A7542922C9902259CC9";
struct PropertyTextArguments {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const PropertyTextArguments&) const = default;
};
inline bool validPropertyTextArguments(const PropertyTextArguments& rect)
{ return rect.right > rect.left && rect.bottom > rect.top; }
struct PropertyTextSample {
    propertyTextControl::Identity identity;
    PropertyTextArguments arguments;
    ULONGLONG calls{};
};
struct PropertyTextProbeCounts {
    ULONGLONG accepted{}, mismatches{}, readFailures{}, capacityDrops{}, busyDrops{};
    std::array<ULONGLONG, static_cast<size_t>(propertyDescription::Stage::Count)> rejectedStages{};
};
struct PropertyTextProbePatch { DWORD originalProtection{}, finalProtection{}, cleanupError{}; };
inline constexpr size_t propertyTextDiagnosticReserve = 16;
struct PropertyTextDiagnosticGroup {
    propertyTextControl::Diagnostic key{};
    ULONGLONG calls{};
};
struct PropertyTextDiagnostics {
    std::array<DWORD, 3> moduleSizes{};
    DWORD overflow{};
    std::array<ULONGLONG, static_cast<size_t>(propertyTextControl::Reason::Count)> reasons{};
    std::array<ULONGLONG, 2> drops{};
    std::array<DWORD, 2> used{};
    std::array<PropertyTextDiagnosticGroup, propertyTextDiagnosticReserve * 2> groups{};
};
struct PropertyTextProbeTrace {
    DWORD size{sizeof(PropertyTextProbeTrace)}, version{propertyTextProbeVersion};
    DWORD sampleCount{}, elapsedMs{}, patchError{}, snapshotError{};
    PropertyTextProbePatch patch;
    PropertyTextProbeCounts counts;
    PropertyTextDiagnostics diagnostics;
    std::array<PropertyTextSample, propertyTextProbeCapacity> samples{};
};
static_assert(sizeof(PropertyTextSample) == 40);
static_assert(sizeof(PropertyTextDiagnosticGroup) == 24);
static_assert(sizeof(PropertyTextDiagnostics) == 992);
static_assert(sizeof(PropertyTextProbeTrace) == 6320);
