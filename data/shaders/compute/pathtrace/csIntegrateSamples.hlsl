// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"
cbuffer C : register(b0)
{
    PathTraceCacheIntegrateSamplesConstants g;
}

[RootSignature(ROOT_SIG)]
[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint sampleIdx = dtid.x;
    if (sampleIdx >= g.samplesCount)
    {
        return;
    }

    StructuredBuffer<RadianceSample> samples = ResourceDescriptorHeap[g.radianceSamplesSrvIndex];
    RWStructuredBuffer<RadianceCacheEntry> cache = ResourceDescriptorHeap[g.radianceCacheUavIndex];

    RadianceSample s = samples[sampleIdx];
    if (s.key == RC_EMPTY)
    {
        return;
    }

    // Open addressing into the cache; keep probes bounded.
    uint idx = s.key & RC_MASK;

    [loop]
    for (uint p = 0; p < g.maxProbes; ++p)
    {
        uint i = (idx + p) & RC_MASK;
        uint existingKey = cache[i].key;

        // Age-out stale entries but never touch EMPTY/LOCKED markers.
        if (existingKey != RC_EMPTY && existingKey != RC_LOCKED)
        {
            uint age = g.frameIndex - cache[i].lastFrame;
            if (age > g.maxAge)
            {
                uint oldKey;
                InterlockedCompareExchange(cache[i].key, existingKey, RC_EMPTY, oldKey);

                if (oldKey == existingKey)
                {
                    existingKey = RC_EMPTY;
                }
                else
                {
                    existingKey = oldKey;
                }
            }
        }

        // Try to claim an empty slot for this key.
        if (existingKey == RC_EMPTY)
        {
            uint oldKey;
            InterlockedCompareExchange(cache[i].key, RC_EMPTY, RC_LOCKED, oldKey);

            if (oldKey == RC_EMPTY)
            {
                cache[i].normalOct = 0;
                cache[i].radianceR = s.radianceR;
                cache[i].radianceG = s.radianceG;
                cache[i].radianceB = s.radianceB;
                cache[i].lastFrame = g.frameIndex;
                cache[i].sampleCount = 1;

                DeviceMemoryBarrier();

                uint dummy;
                InterlockedExchange(cache[i].key, s.key, dummy);
                return;
            }

            existingKey = oldKey;
        }

        // Another thread is initializing this slot.
        if (existingKey == RC_LOCKED)
        {
            continue;
        }

        // Accumulate into the matching key; clamp sampleCount.
        if (existingKey == s.key)
        {
            uint oldCount;
            InterlockedAdd(cache[i].sampleCount, 1, oldCount);

            if (oldCount >= g.maxSamples)
            {
                InterlockedAdd(cache[i].sampleCount, 0xFFFFFFFFu);
                cache[i].lastFrame = g.frameIndex;
                return;
            }

            InterlockedAdd(cache[i].radianceR, s.radianceR);
            InterlockedAdd(cache[i].radianceG, s.radianceG);
            InterlockedAdd(cache[i].radianceB, s.radianceB);

            cache[i].lastFrame = g.frameIndex;
            return;
        }
    }
}
