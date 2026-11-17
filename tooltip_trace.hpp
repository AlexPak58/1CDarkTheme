#pragma once

#include "platform.hpp"

struct TooltipTraceColor {
    DWORD rgba{};
    DWORD calls{};
};

struct TooltipTrace {
    std::array<TooltipTraceColor, 16> colors{};
    DWORD otherCalls{};
};

static_assert(sizeof(TooltipTrace) == 132);
