// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"
cbuffer C : register(b0)
{
    PathTraceCachePruneConstants g;
}

uint HashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[RootSignature(ROOT_SIG)]
[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    RWStructuredBuffer<RadianceCacheEntry> cache = ResourceDescriptorHeap[g.radianceCacheUavIndex];
    RWStructuredBuffer<u32> cacheUsageCounter = ResourceDescriptorHeap[g.radianceCacheUsageCounterUavIndex];

    uint seed = HashUint(dtid.x ^ g.randomSeed);
    uint attempts = max(1u, g.attemptsPerThread);

    [loop]
    for (uint a = 0; a < attempts; ++a)
    {
        seed = HashUint(seed + (a + 1u) * 0x9E3779B9u);
        uint i = seed & RC_MASK;

        uint key = cache[i].key;
        if (key == RC_EMPTY || key == RC_LOCKED)
        {
            continue;
        }

        uint age = g.frameIndex - cache[i].lastFrame;
        uint sampleCount = cache[i].sampleCount;

        // Keep hot entries unless pressure is high enough to bypass these filters.
        if (age < g.minAgeToPrune && sampleCount >= g.minSamplesToKeep)
        {
            continue;
        }

        uint oldKey;
        InterlockedCompareExchange(cache[i].key, key, RC_EMPTY, oldKey);
        if (oldKey == key)
        {
            InterlockedAdd(cacheUsageCounter[0], 0xFFFFFFFFu);
            return;
        }
    }
}

