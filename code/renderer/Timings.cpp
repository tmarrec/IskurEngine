// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Timings.h"

#include <algorithm>
#include <pix.h>

namespace
{
void PushHistorySample(TimingState::TimingSmoother& smoother, const double sample, const float dtMs)
{
    const u32 capacity = TimingState::kHistoryCapacity;
    if (capacity == 0u)
    {
        return;
    }

    if (smoother.history.empty())
    {
        smoother.history.resize(capacity);
    }

    if (smoother.historyCount < capacity)
    {
        const u32 writeIndex = (smoother.historyStart + smoother.historyCount) % capacity;
        smoother.history[writeIndex].value = sample;
        smoother.history[writeIndex].dtMs = dtMs;
        ++smoother.historyCount;
        return;
    }

    smoother.history[smoother.historyStart].value = sample;
    smoother.history[smoother.historyStart].dtMs = dtMs;
    smoother.historyStart = (smoother.historyStart + 1u) % capacity;
}

double ComputeBoxAverage(const TimingState::TimingSmoother& smoother, const float windowMs)
{
    if (smoother.historyCount == 0u)
    {
        return 0.0;
    }

    if (windowMs <= 0.0f)
    {
        const u32 newestIndex = (smoother.historyStart + smoother.historyCount - 1u) % TimingState::kHistoryCapacity;
        return smoother.history[newestIndex].value;
    }

    double weightedSum = 0.0;
    double coveredMs = 0.0;

    for (u32 age = 0; age < smoother.historyCount && coveredMs < windowMs; ++age)
    {
        const u32 idx = (smoother.historyStart + smoother.historyCount - 1u - age) % TimingState::kHistoryCapacity;
        const TimingState::TimingSample& entry = smoother.history[idx];
        const double sampleDtMs = entry.dtMs > 0.0f ? static_cast<double>(entry.dtMs) : 0.0;
        if (sampleDtMs <= 0.0)
        {
            continue;
        }

        const double remainingMs = static_cast<double>(windowMs) - coveredMs;
        const double takeMs = std::min(sampleDtMs, remainingMs);

        weightedSum += entry.value * takeMs;
        coveredMs += takeMs;
    }

    if (coveredMs <= 0.0)
    {
        const u32 newestIndex = (smoother.historyStart + smoother.historyCount - 1u) % TimingState::kHistoryCapacity;
        return smoother.history[newestIndex].value;
    }

    return weightedSum / coveredMs;
}
} // namespace

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

void CPU_MARKER_BEGIN(CpuTimers& timers, const char* name)
{
    if (timers.openScopeCount >= 128)
    {
        return;
    }

    CpuTimers::Scope& s = timers.openScopes[timers.openScopeCount++];
    s.name = name;
    s.startTime = std::chrono::steady_clock::now();
}

void CPU_MARKER_END(CpuTimers& timers)
{
    if (timers.openScopeCount == 0)
    {
        return;
    }

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const CpuTimers::Scope s = timers.openScopes[--timers.openScopeCount];
    const f64 dtMs = std::chrono::duration<f64, std::milli>(endTime - s.startTime).count();

    if (timers.passCount >= 128)
    {
        return;
    }

    CpuTimers::Pass& p = timers.passes[timers.passCount++];
    p.name = s.name;
    p.ms = dtMs;
}

void Timings_UpdateAverages(TimingState& s, float dtMs, float windowMs)
{
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

        if (j == s.smoothCount && j < TimingState::kMaxTimings)
        {
            s.smooth[j].name = name;
            s.smoothCount++;
        }

        auto& sm = s.smooth[j];
        PushHistorySample(sm, sample, dtMs);
        sm.value = ComputeBoxAverage(sm, windowMs);
        sm.initialized = true;
    }
}

double Timings_ComputeAverageWindowMs(const TimingState& s, const char* name, float windowMs)
{
    for (u32 i = 0; i < s.smoothCount; ++i)
    {
        const auto& sm = s.smooth[i];
        if (sm.name == name)
        {
            return ComputeBoxAverage(sm, windowMs);
        }
    }

    return 0.0;
}

void Timings_ResetAverages(TimingState& s)
{
    s.lastCount = 0;
    s.smoothCount = 0;
    for (auto& entry : s.smooth)
    {
        entry.name = nullptr;
        entry.value = 0.0;
        entry.initialized = false;
        entry.historyStart = 0;
        entry.historyCount = 0;
    }
}

void Timings_CollectGpu(const GpuTimers& timers, TimingState& s)
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

    for (u32 i = 0; i < timers.passCount && i < TimingState::kMaxTimings; ++i)
    {
        const GpuTimers::Pass& p = timers.passes[i];
        if (p.idxEnd <= p.idxBegin)
        {
            continue;
        }
        const u64 t0 = ticks[p.idxBegin];
        const u64 t1 = ticks[p.idxEnd];
        const f64 dt = static_cast<f64>(t1 - t0) * to_ms;

        TimingState::TimDisp& ts = s.last[s.lastCount++];
        ts.name = p.name;
        ts.ms = dt;
    }

    D3D12_RANGE w{0, 0};
    timers.readback->Unmap(0, &w);
}

void Timings_CollectCpu(const CpuTimers& timers, TimingState& s)
{
    s.lastCount = 0;
    for (u32 i = 0; i < timers.passCount && i < TimingState::kMaxTimings; ++i)
    {
        TimingState::TimDisp& ts = s.last[s.lastCount++];
        ts.name = timers.passes[i].name;
        ts.ms = timers.passes[i].ms;
    }
}

