#pragma once

#include "platform.hpp"

inline constexpr std::array<ULONG_PTR, 4> searchGridWhiteWriteRvas{
    0x2F0997, 0x2F0B76, 0x2F0E13, 0x2F0EA0,
};

struct SearchResultsTrace {
    DWORD cachedColorCalls;
    DWORD originalColorCalls;
    DWORD unexpectedColorCalls;
    DWORD otherGridCalls;
    DWORD cachedColorCopies;
    DWORD otherGridColorCopies;
    std::array<DWORD, searchGridWhiteWriteRvas.size()> whiteWrites;
    DWORD resultsStandaloneCreations;
    DWORD resultsAggregatedCreations;
    DWORD otherCreations;
    DWORD resultsBackgrounds;
    DWORD otherBackgrounds;
    DWORD replacedCairoFills;
    DWORD replacedRowFills;
};

static_assert(sizeof(SearchResultsTrace) == (13 + searchGridWhiteWriteRvas.size()) * sizeof(DWORD));
