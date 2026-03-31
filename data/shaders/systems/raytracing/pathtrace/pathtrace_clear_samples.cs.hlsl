// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"

cbuffer C : register(b0)
{
    PathTraceCacheClearSamplesConstants g;
}

[RootSignature(ROOT_SIG)]
[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= g.samplesCount)
    {
        return;
    }

    RWStructuredBuffer<RadianceSample> samples = ResourceDescriptorHeap[g.radianceSamplesUavIndex];

    RadianceSample s;
    s.key = RC_EMPTY;
    s.keySignature = 0;
    s.radianceR = 0;
    s.radianceG = 0;
    s.radianceB = 0;
    samples[idx] = s;
}

