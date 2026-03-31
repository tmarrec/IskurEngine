// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

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
    RWStructuredBuffer<u32> cacheUsageCounter = ResourceDescriptorHeap[g.radianceCacheUsageCounterUavIndex];

    RadianceSample s = samples[sampleIdx];
    if (s.key == RC_EMPTY)
    {
        return;
    }

    u32 safePerSample = (g.maxSamples > 0u) ? (0xFFFFFFFFu / g.maxSamples) : 0xFFFFFFFFu;
    u32 sampleR = min(s.radianceR, safePerSample);
    u32 sampleG = min(s.radianceG, safePerSample);
    u32 sampleB = min(s.radianceB, safePerSample);

    // Open addressing into the cache; keep probes bounded.
    uint idx = s.key & RC_MASK;
    uint replaceIdx = RC_EMPTY;
    uint replaceKey = RC_EMPTY;
    uint replaceAge = 0u;
    uint replaceSamples = 0xFFFFFFFFu;

    [loop]
    for (uint p = 0; p < g.maxProbes; ++p)
    {
        uint i = (idx + p) & RC_MASK;
        uint existingKey = cache[i].key;

        // Try to claim an empty slot for this key.
        if (existingKey == RC_EMPTY)
        {
            uint oldKey;
            InterlockedCompareExchange(cache[i].key, RC_EMPTY, RC_LOCKED, oldKey);

            if (oldKey == RC_EMPTY)
            {
                cache[i].normalOct = s.keySignature;
                cache[i].radianceR = sampleR;
                cache[i].radianceG = sampleG;
                cache[i].radianceB = sampleB;
                cache[i].lastFrame = g.frameIndex;
                cache[i].sampleCount = 1;

                DeviceMemoryBarrier();

                uint dummy;
                InterlockedExchange(cache[i].key, s.key, dummy);
                InterlockedAdd(cacheUsageCounter[0], 1);
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
            if (cache[i].normalOct != s.keySignature)
            {
                continue;
            }

            uint oldCount;
            InterlockedAdd(cache[i].sampleCount, 1, oldCount);

            if (oldCount >= g.maxSamples)
            {
                InterlockedAdd(cache[i].sampleCount, 0xFFFFFFFFu);
                cache[i].lastFrame = g.frameIndex;
                return;
            }

            InterlockedAdd(cache[i].radianceR, sampleR);
            InterlockedAdd(cache[i].radianceG, sampleG);
            InterlockedAdd(cache[i].radianceB, sampleB);

            cache[i].lastFrame = g.frameIndex;
            return;
        }

        // Track a replacement candidate in case this key cannot be inserted
        // within the bounded probe window (persistent miss/flicker otherwise).
        RadianceCacheEntry e = cache[i];
        uint age = g.frameIndex - e.lastFrame;
        if (replaceIdx == RC_EMPTY || age > replaceAge || (age == replaceAge && e.sampleCount < replaceSamples))
        {
            replaceIdx = i;
            replaceKey = existingKey;
            replaceAge = age;
            replaceSamples = e.sampleCount;
        }
    }

    // Probe window full and no match found: recycle one stale entry so keys
    // don't starve forever in crowded hash neighborhoods.
    if (replaceIdx != RC_EMPTY)
    {
        uint oldKey;
        InterlockedCompareExchange(cache[replaceIdx].key, replaceKey, RC_LOCKED, oldKey);
        if (oldKey == replaceKey)
        {
            cache[replaceIdx].normalOct = s.keySignature;
            cache[replaceIdx].radianceR = sampleR;
            cache[replaceIdx].radianceG = sampleG;
            cache[replaceIdx].radianceB = sampleB;
            cache[replaceIdx].lastFrame = g.frameIndex;
            cache[replaceIdx].sampleCount = 1;

            DeviceMemoryBarrier();

            uint dummy;
            InterlockedExchange(cache[replaceIdx].key, s.key, dummy);
        }
    }
}

