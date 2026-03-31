// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "TemporalJitter.h"

namespace
{
f32 Halton(u32 index, u32 base)
{
    f32 f = 1.0f;
    f32 r = 0.0f;
    while (index > 0)
    {
        f /= static_cast<f32>(base);
        r += f * static_cast<f32>(index % base);
        index /= base;
    }
    return r;
}
} // namespace

TemporalJitter::Sample TemporalJitter::ComputeHaltonJitter(u32 frameIndex, u32 renderWidth, u32 renderHeight, u32 phaseCount)
{
    IE_Assert(renderWidth > 0);
    IE_Assert(renderHeight > 0);
    IE_Assert(phaseCount > 0);

    const u32 jitterIndex = (frameIndex % phaseCount) + 1;
    const f32 jitterPxX = Halton(jitterIndex, 2) - 0.5f;
    const f32 jitterPxY = Halton(jitterIndex, 3) - 0.5f;

    Sample jitter;
    jitter.jitterNormX = jitterPxX * 2.0f / static_cast<f32>(renderWidth);
    jitter.jitterNormY = -jitterPxY * 2.0f / static_cast<f32>(renderHeight);
    return jitter;
}
