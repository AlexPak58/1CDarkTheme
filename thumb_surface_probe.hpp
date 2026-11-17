#pragma once

#include "platform.hpp"
#include <type_traits>

inline constexpr DWORD thumbSurfaceProbeVersion = 5;
inline constexpr DWORD thumbSurfaceCaptureMs = 60000;
inline constexpr size_t thumbSurfaceCapacity = 128;
inline constexpr auto thumbSurfaceImageUiHash =
    L"C807765D1C990287EDDF752A53F7E0335269AF2B8ADD4FCE20920ED99E387846";
inline constexpr auto thumbSurfaceImageHash =
    L"5F7F474CA58EDAEC4BC12478432D7D56C7EC092A92CED94FA0C0873B5D38B288";
inline constexpr char thumbSurfaceRunExport[] = "RunThumbSurfaceProbeV5";
inline constexpr char thumbSurfaceAbiExport[] = "ThumbSurfaceProbeAbiV5";

enum class ThumbSurfaceStage : DWORD {
    OtherCall, GraphicsChain, DrawReturn, DelegatedDraw, DelegatedFrames, PaintReturn, OwnerReturn,
    OuterReturn, Vdc, Orientation, ThumbPresent, Owner, Component, Cache, State,
    ReadFailure, SurfaceError, SurfaceType, InvalidMetrics, Accepted, Count
};

// A call reaching the cache gate records its first input/table read failure,
// its first invalid input, or exactly one table pair. A known pair additionally
// records one method outcome. These counters do not change stage admission.
enum class ThumbCacheCounter : size_t {
    InputsReadFailure, NullCache, InvalidDrawQi, DrawingObjectMismatch, NullRasterQi, TablesReadFailure,
    OriginalPair, AlternateDrawPair, AlternateRasterPair, AlternateBothPair, UnknownPair,
    MethodsReadFailure, WrongDrawMethod, WrongRasterMethod, WrongBothMethods, MethodsMatch, Count
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

// Coordinates are independent intervals, not an assertion that every combination occurred.
using ThumbSurfaceGeometry = std::array<LONG, 8>;
inline ThumbSurfaceGeometry thumbSurfaceGeometry(const ThumbSurfaceIdentity& id)
{
    return {id.left, id.top, id.right, id.bottom,
            id.thumbLeft, id.thumbTop, id.thumbRight, id.thumbBottom};
}

inline bool sameThumbSurfaceGroup(ThumbSurfaceIdentity a, ThumbSurfaceIdentity b)
{
    // Keep every semantic field in the key; omit only absolute paint coordinates.
    a.left = a.top = a.right = a.bottom = 0;
    a.thumbLeft = a.thumbTop = a.thumbRight = a.thumbBottom = 0;
    b.left = b.top = b.right = b.bottom = 0;
    b.thumbLeft = b.thumbTop = b.thumbRight = b.thumbBottom = 0;
    return a == b;
}

struct ThumbSurfaceSample {
    ThumbSurfaceIdentity identity;
    ThumbSurfaceGeometry geometryMin{}, geometryMax{};
    ULONGLONG calls{};
};

struct ThumbSurfaceTrace {
    DWORD size{sizeof(ThumbSurfaceTrace)}, version{thumbSurfaceProbeVersion};
    DWORD sampleCount{}, elapsedMs{}, startError{}, endError{}, snapshotError{}, reserved{};
    ULONGLONG busyDrops{}, capacityDrops{};
    std::array<ULONGLONG, static_cast<size_t>(ThumbSurfaceStage::Count)> stages{};
    std::array<ULONGLONG, static_cast<size_t>(ThumbCacheCounter::Count)> cacheCounters{};
    std::array<ThumbSurfaceSample, thumbSurfaceCapacity> samples{};
};

static_assert(sizeof(ThumbSurfaceIdentity) == 136);
static_assert(sizeof(ThumbSurfaceSample) == 208);
static_assert(sizeof(ThumbSurfaceTrace) == 26960);
static_assert(std::is_standard_layout_v<ThumbSurfaceTrace> && std::is_trivially_copyable_v<ThumbSurfaceTrace>);
