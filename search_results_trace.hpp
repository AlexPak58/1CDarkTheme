#pragma once

#include "platform.hpp"

struct SearchResultsTrace {
    DWORD cachedColorCalls;
    DWORD originalColorCalls;
    DWORD unexpectedColorCalls;
    DWORD otherGridCalls;
};

static_assert(sizeof(SearchResultsTrace) == 4 * sizeof(DWORD));
