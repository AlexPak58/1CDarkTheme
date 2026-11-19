#pragma once

#include "platform.hpp"

inline constexpr DWORD pngSurfaceProbeVersion = 1;
inline constexpr DWORD pngSurfaceCaptureMs = 60000;
inline constexpr size_t pngSurfaceCapacity = 128;
inline constexpr ULONG_PTR pngSurfaceIatRva = 0xC7BE0;
inline constexpr ULONG_PTR pngSurfaceOriginalRva = 0x34BA0;
inline constexpr char pngSurfaceRunExport[] = "RunScrollerPngSurfaceProbeV1";
inline constexpr char pngSurfaceAbiExport[] = "ScrollerPngSurfaceProbeAbiV1";
inline constexpr auto pngSurfaceCairoHash =
    L"8B790F026318882AB0A054E6228B1C0C0A210512582C6D6D001E2EFB42256109";

enum class PngSurfaceStage : DWORD {
    OtherCall, GraphicsChain, PaintRoute, Owner, Component, ReadFailure,
    SurfaceError, SurfaceType, InvalidMetrics, Accepted, Count
};

// Numeric metadata only: no surface, pixel, owner or transient heap pointers.
struct PngSurfaceIdentity {
    DWORD windowTableRva{}, scrollerTableRva{}, componentOffset{}, part{};
    DWORD orientation{}, hotId{}, pressedId{}, reserved{};
    LONG left{}, top{}, right{}, bottom{};
    LONG surfaceType{}, format{}, width{}, height{}, stride{}, status{};
    double deviceScaleX{}, deviceScaleY{}, deviceOffsetX{}, deviceOffsetY{};
    bool operator==(const PngSurfaceIdentity&) const = default;
};
struct PngSurfaceSample {
    PngSurfaceIdentity identity;
    ULONGLONG calls{};
};
struct PngSurfaceTrace {
    DWORD size{sizeof(PngSurfaceTrace)}, version{pngSurfaceProbeVersion};
    DWORD sampleCount{}, elapsedMs{}, patchError{}, snapshotError{}, cleanupError{};
    DWORD originalProtection{}, finalProtection{}, reserved{};
    ULONGLONG busyDrops{}, capacityDrops{};
    std::array<ULONGLONG, static_cast<size_t>(PngSurfaceStage::Count)> stages{};
    std::array<PngSurfaceSample, pngSurfaceCapacity> samples{};
};

static_assert(sizeof(PngSurfaceIdentity) == 104);
static_assert(sizeof(PngSurfaceSample) == 112);
static_assert(sizeof(PngSurfaceTrace) == 14472);
