// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<HistogramConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=6, b0)"

static const uint MAX_BUCKETS = 256;
groupshared uint sHist[MAX_BUCKETS];

[RootSignature(ROOT_SIG)]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gtid : SV_GroupThreadID)
{
    Texture2D<float4> hdrTexture = ResourceDescriptorHeap[Constants.hdrTextureIndex];
    RWStructuredBuffer<uint> histogramGlobal = ResourceDescriptorHeap[Constants.histogramBufferIndex];

    const uint linearTid = gtid.y * 16 + gtid.x;
    if (linearTid < Constants.numBuckets)
    {
        sHist[linearTid] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint width, height;
    hdrTexture.GetDimensions(width, height);
    uint2 pixel = tid.xy;

    if (pixel.x < width && pixel.y < height)
    {
        float3 hdr = hdrTexture.Load(int3(pixel, 0)).rgb;

        float lum = dot(hdr, float3(0.2126f, 0.7152f, 0.0722f));
        float logL = clamp(log2(max(lum, 1e-5f)), Constants.minLogLum, Constants.maxLogLum);

        float invRange = rcp(max(Constants.maxLogLum - Constants.minLogLum, 1e-6f));
        float t = saturate((logL - Constants.minLogLum) * invRange);
        uint idx = (uint)(t * (float)(Constants.numBuckets - 1) + 0.5f);
        idx = (idx < Constants.numBuckets) ? idx : (Constants.numBuckets - 1);

        // Center-weighted metering: prioritize middle pixels, de-emphasize edges/corners.
        float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
        float2 centered = uv * 2.0f - 1.0f;
        float r2 = dot(centered, centered);
        float centerW = saturate(1.0f - r2 * 0.5f);
        centerW *= centerW;

        // Integer accumulation for weighted histogram.
        uint weight = (uint)(centerW * 8.0f + 0.5f);
        if (weight > 0u)
        {
            InterlockedAdd(sHist[idx], weight);
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

