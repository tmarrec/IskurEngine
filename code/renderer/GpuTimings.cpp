// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "GpuTimings.h"

#include <algorithm>
#include <pix.h>

void GPU_MARKER_BEGIN(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers, const char* name)
{
    PIXBeginEvent(cmd.Get(), 0, name);
    u32 pi = timers.passCount++;
    timers.passes[pi].name = name;
    timers.passes[pi].idxBegin = timers.nextIdx++;
    cmd->EndQuery(timers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timers.passes[pi].idxBegin);
}

void GPU_MARKER_END(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& timers)
{
    GpuTimers::Pass& p = timers.passes[timers.passCount - 1];
    p.idxEnd = timers.nextIdx++;
    cmd->EndQuery(timers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p.idxEnd);
    PIXEndEvent(cmd.Get());
}

void GpuTimings_UpdateAverages(GpuTimingState& s, float dtMs, float windowMs)
{
    float alpha = windowMs <= 0.0f ? 1.0f : dtMs / windowMs;
    alpha = std::clamp(alpha, 0.f, 1.f);

    for (u32 i = 0; i < s.lastCount; ++i)
    {
        const char* name = s.last[i].name;
        double sample = s.last[i].ms;

        u32 j = 0;
        for (; j < s.smoothCount; ++j)
        {
            if (s.smooth[j].name == name)
                break;
        }

        if (j == s.smoothCount && j < 128)
        {
            s.smooth[j].name = name;
            s.smooth[j].value = sample;
            s.smooth[j].initialized = true;
            s.smoothCount++;
            continue;
        }

        auto& sm = s.smooth[j];
        if (!sm.initialized)
        {
            sm.value = sample;
            sm.initialized = true;
        }
        else
        {
            sm.value += (sample - sm.value) * alpha;
        }
    }
}

void GpuTimings_Collect(const GpuTimers& timers, GpuTimingState& s)
{
    s.lastCount = 0;
    if (!timers.nextIdx || !s.timestampFrequency)
    {
        return;
    }
    const f64 to_ms = 1000.0 / static_cast<f64>(s.timestampFrequency);

    u64* ticks = nullptr;
    D3D12_RANGE r{0, static_cast<size_t>(timers.nextIdx) * sizeof(u64)};
    IE_Check(timers.readback->Map(0, &r, reinterpret_cast<void**>(&ticks)));

    for (u32 i = 0; i < timers.passCount && i < 128; ++i)
    {
        const GpuTimers::Pass& p = timers.passes[i];
        if (p.idxEnd <= p.idxBegin)
        {
            continue;
        }
        const u64 t0 = ticks[p.idxBegin];
        const u64 t1 = ticks[p.idxEnd];
        const f64 dt = static_cast<f64>(t1 - t0) * to_ms;

        GpuTimingState::TimDisp& ts = s.last[s.lastCount++];
        ts.name = p.name;
        ts.ms = dt;
    }

    D3D12_RANGE w{0, 0};
    timers.readback->Unmap(0, &w);
}
