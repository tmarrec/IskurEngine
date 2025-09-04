// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <wrl/client.h>

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

// Aggregation/smoothing state
struct GpuTimingState
{
    u64 timestampFrequency = 0; // ticks/sec from command queue

    struct TimDisp
    {
        const char* name;
        f64 ms;
    } last[128];
    u32 lastCount = 0;

    struct TimingSmoother
    {
        const char* name = nullptr; // expected to be a stable literal
        double value = 0.0;
        bool initialized = false;
    };
    TimingSmoother smooth[128] = {};
    u32 smoothCount = 0;
};

void GPU_MARKER_BEGIN(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers, const char* name);
void GPU_MARKER_END(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers);

// Update exponential moving averages for the last timings
void GpuTimings_UpdateAverages(GpuTimingState& s, float dtMs, float windowMs);

// Read back timestamp data from the given timers and populate the "last" timings in the state
void GpuTimings_Collect(GpuTimers& timers, GpuTimingState& s);
