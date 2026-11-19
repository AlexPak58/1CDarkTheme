#pragma once

#include "platform.hpp"

inline constexpr DWORD gridProbeVersion = 1;
inline constexpr size_t gridProbeCapacity = 16;
inline constexpr DWORD gridProbeRva = 0x8118A8;
inline constexpr DWORD gridWrapperReturnRva = 0x8117F5;
inline constexpr DWORD gridWrapperStackOffset = 0xC8;
inline constexpr DWORD gridStandaloneTableRva = 0xA8A278;
inline constexpr DWORD gridAggregatedTableRva = 0xA88AC0;
inline constexpr DWORD gridProviderField = 0x7B0;
inline constexpr DWORD gridRowsField = 0x7F0;
inline constexpr DWORD gridSiteField = 8;
inline constexpr DWORD gridRowsAliasOffset = 0x18;
inline constexpr std::array<BYTE, 16> gridProbeSignature{
    0x48, 0x8B, 0xF9, 0x41, 0xB4, 0x01, 0x80, 0xB9,
    0x88, 0x0A, 0x00, 0x00, 0x00, 0x74, 0x13, 0x48};
inline constexpr char gridProbeExport[] = "RunGridIdentityProbeV1";
inline constexpr char gridProbeAbiExport[] = "GridIdentityProbeAbiV1";

enum class GridTableState : DWORD { NotRead, Null, ReadFailed, UnmappedTable, ImageTable };
enum class GridRootKind : DWORD { Unknown, Standalone, Aggregated };
enum class GridRowsAlias : DWORD { Unknown, Separate, Same, ProviderPlusOffset };

// Only immutable image tables have a location. Object addresses and contents
// other than the bounded pointer fields never enter this result.
struct GridTableIdentity {
    ULONG_PTR imageBase{};
    DWORD rva{};
    GridTableState state{};
    bool operator==(const GridTableIdentity&) const = default;
};

struct GridIdentity {
    GridTableIdentity root;
    GridTableIdentity provider;
    GridTableIdentity rows;
    GridTableIdentity site;
    GridRootKind kind{};
    GridRowsAlias alias{};
    bool operator==(const GridIdentity&) const = default;
};

struct GridProbeSample {
    GridIdentity identity;
    ULONGLONG calls{};
};

struct GridProbeTrace {
    DWORD size{sizeof(GridProbeTrace)};
    DWORD version{gridProbeVersion};
    DWORD sampleCount{};
    DWORD elapsedMs{};
    DWORD originalProtection{};
    DWORD finalProtection{};
    DWORD patchError{};
    DWORD cleanupError{};
    ULONGLONG totalCalls{};
    ULONGLONG droppedCalls{};
    ULONGLONG otherCallerCalls{};
    ULONGLONG stackReadFailures{};
    std::array<GridProbeSample, gridProbeCapacity> samples{};
};

static_assert(sizeof(GridTableIdentity) == 16);
static_assert(sizeof(GridIdentity) == 72);
static_assert(sizeof(GridProbeSample) == 80);
static_assert(sizeof(GridProbeTrace) == 1344);
