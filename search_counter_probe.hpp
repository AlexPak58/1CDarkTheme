#pragma once

#include "platform.hpp"

inline constexpr DWORD counterProbeVersion = 1;
inline constexpr size_t counterProbeCapacity = 16;
inline constexpr DWORD counterProbeRva = 0x285805;
inline constexpr std::array<BYTE, 6> counterProbeSignature{0x48, 0x8B, 0xCF, 0xFF, 0x50, 0x40};
inline constexpr char counterProbeExport[] = "RunSearchCounterProbeV1";
inline constexpr char counterProbeAbiExport[] = "SearchCounterProbeAbiV1";

struct CounterProbeSample {
    ULONG_PTR receiver{};
    ULONG_PTR table{};
    DWORD thread{};
    DWORD calls{};
};

struct CounterProbeTrace {
    DWORD size{sizeof(CounterProbeTrace)};
    DWORD version{counterProbeVersion};
    DWORD sampleCount{};
    DWORD totalCalls{};
    DWORD droppedCalls{};
    DWORD elapsedMs{};
    DWORD originalProtection{};
    DWORD finalProtection{};
    DWORD patchError{};
    DWORD cleanupError{};
    std::array<CounterProbeSample, counterProbeCapacity> samples{};
};

static_assert(sizeof(CounterProbeSample) == 24);
static_assert(sizeof(CounterProbeTrace) == 424);
