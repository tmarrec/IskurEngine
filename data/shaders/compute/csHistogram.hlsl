// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.

#include "CPUGPU.h"

ConstantBuffer<HistogramConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=6, b0)"

static const uint MAX_BUCKETS = 256;
groupshared uint sHist[MAX_BUCKETS];

[RootSignature(ROOT_SIG)]
[NumThreads(16,16,1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint3 gtid : SV_GroupThreadID)
{
    Texture2D<float4> hdrTexture = ResourceDescriptorHeap[Constants.hdrTextureIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    RWStructuredBuffer<uint> histogramGlobal = ResourceDescriptorHeap[Constants.histogramBufferIndex];

    // Clear the group-shared histogram
    const uint linearTid = gtid.y * 16 + gtid.x;
    if (linearTid < Constants.numBuckets)
    {
        sHist[linearTid] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Process one pixel per thread
    uint width, height;
    hdrTexture.GetDimensions(width, height);
    uint2 pixel = tid.xy;

    if (pixel.x < width && pixel.y < height)
    {
        float d = depthTex.Load(int3(pixel, 0));
        if (d > 1e-6f)
        {
            float3 hdr = hdrTexture.Load(int3(pixel, 0)).rgb;

            // Luminance + log2
            float lum = dot(hdr, float3(0.2126f, 0.7152f, 0.0722f));
            float logL = clamp(log2(max(lum, 1e-5f)), Constants.minLogLum, Constants.maxLogLum);

            // Precompute a reciprocal to avoid a divide; keep numerics stable
            float invRange = rcp(max(Constants.maxLogLum - Constants.minLogLum, 1e-6f));
            float t = saturate((logL - Constants.minLogLum) * invRange);

            // Map to [0..numBuckets-1] with rounding, then clamp once more
            uint idx = (uint) (t * (float)(Constants.numBuckets - 1) + 0.5f);
            idx = (idx < Constants.numBuckets) ? idx : (Constants.numBuckets - 1);

            InterlockedAdd(sHist[idx], 1);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (linearTid < Constants.numBuckets)
    {
        uint count = sHist[linearTid];
        if (count != 0)
        {
            InterlockedAdd(histogramGlobal[linearTid], count);
        }
    }
}
