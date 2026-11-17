#pragma once

#include "platform.hpp"
#include <type_traits>

inline constexpr DWORD thumbSurfaceProbeVersion = 2;
inline constexpr DWORD thumbSurfaceCaptureMs = 60000;
inline constexpr size_t thumbSurfaceCapacity = 128;
inline constexpr auto thumbSurfaceImageUiHash =
    L"C807765D1C990287EDDF752A53F7E0335269AF2B8ADD4FCE20920ED99E387846";
inline constexpr auto thumbSurfaceImageHash =
    L"5F7F474CA58EDAEC4BC12478432D7D56C7EC092A92CED94FA0C0873B5D38B288";
inline constexpr char thumbSurfaceRunExport[] = "RunThumbSurfaceProbeV2";
inline constexpr char thumbSurfaceAbiExport[] = "ThumbSurfaceProbeAbiV2";

enum class ThumbSurfaceStage : DWORD {
    OtherCall, GraphicsChain, DrawReturn, DelegatedDraw, DelegatedFrames, PaintReturn, OwnerReturn,
    OuterReturn, Vdc, Orientation, ThumbPresent, Owner, Component, Cache, State,
    ReadFailure, SurfaceError, SurfaceType, InvalidMetrics, Accepted, Count
};

// Numeric metadata only. In particular, do not serialize native object addresses.
struct ThumbSurfaceIdentity {
    DWORD windowTableRva{}, scrollerTableRva{}, componentOffset{}, drawReturnRva{};
    DWORD orientation{}, hotId{}, pressedId{}, cacheState{};
    DWORD enabled{}, interactive{}, reserved1{}, reserved2{};
    LONG left{}, top{}, right{}, bottom{};
    LONG thumbLeft{}, thumbTop{}, thumbRight{}, thumbBottom{};
    LONG surfaceType{}, format{}, width{}, height{}, stride{}, status{};
    double deviceScaleX{}, deviceScaleY{}, deviceOffsetX{}, deviceOffsetY{};
    bool operator==(const ThumbSurfaceIdentity&) const = default;
};

struct ThumbSurfaceSample {
    ThumbSurfaceIdentity identity;
    ULONGLONG calls{};
};

struct ThumbSurfaceTrace {
    DWORD size{sizeof(ThumbSurfaceTrace)}, version{thumbSurfaceProbeVersion};
    DWORD sampleCount{}, elapsedMs{}, startError{}, endError{}, snapshotError{}, reserved{};
    ULONGLONG busyDrops{}, capacityDrops{};
    std::array<ULONGLONG, static_cast<size_t>(ThumbSurfaceStage::Count)> stages{};
    std::array<ThumbSurfaceSample, thumbSurfaceCapacity> samples{};
};

static_assert(sizeof(ThumbSurfaceIdentity) == 136);
static_assert(sizeof(ThumbSurfaceSample) == 144);
static_assert(sizeof(ThumbSurfaceTrace) == 18640);
static_assert(std::is_standard_layout_v<ThumbSurfaceTrace> && std::is_trivially_copyable_v<ThumbSurfaceTrace>);
