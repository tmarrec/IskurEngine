// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <chrono>

#include "common/Types.h"

struct GpuTimers
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    u32 nextIdx = 0;
    struct Pass
    {
        const char* name = nullptr;
        u32 idxBegin = 0;
        u32 idxEnd = 0;
        f64 ms = 0.0;
    } passes[128];
    u32 passCount = 0;
};

struct CpuTimers
{
    struct Scope
    {
        const char* name = nullptr;
        std::chrono::steady_clock::time_point startTime{};
    } openScopes[128];
    u32 openScopeCount = 0;

    struct Pass
    {
        const char* name = nullptr;
        f64 ms = 0.0;
    } passes[128];
    u32 passCount = 0;
};

// Aggregation/smoothing state (shared by CPU and GPU timings).
struct TimingState
{
    static constexpr u32 kMaxTimings = 128u;
    static constexpr u32 kHistoryCapacity = 8192u;

    u64 timestampFrequency = 0; // ticks/sec from command queue (GPU-only)

    struct TimDisp
    {
        const char* name;
        f64 ms;
    } last[kMaxTimings];
    u32 lastCount = 0;

    struct TimingSample
    {
        double value = 0.0;
        float dtMs = 0.0f;
    };

    struct TimingSmoother
    {
        const char* name = nullptr; // expected to be a stable literal
        double value = 0.0;
        bool initialized = false;
        Vector<TimingSample> history{};
        u32 historyStart = 0;
        u32 historyCount = 0;
    };
    TimingSmoother smooth[kMaxTimings] = {};
    u32 smoothCount = 0;
};

void GPU_MARKER_BEGIN(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers, const char* name);
void GPU_MARKER_END(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers);
void CPU_MARKER_BEGIN(CpuTimers& timers, const char* name);
void CPU_MARKER_END(CpuTimers& timers);

// Update strict box averages over the requested time window for the latest timings.
void Timings_UpdateAverages(TimingState& s, float dtMs, float windowMs);

// Compute a strict box average for one timing entry over the requested time window.
double Timings_ComputeAverageWindowMs(const TimingState& s, const char* name, float windowMs);

// Clear timing history/averages while keeping timestamp frequency.
void Timings_ResetAverages(TimingState& s);

// Read back GPU timestamp data and populate the "last" timings in the state.
void Timings_CollectGpu(const GpuTimers& timers, TimingState& s);
void Timings_CollectCpu(const CpuTimers& timers, TimingState& s);
