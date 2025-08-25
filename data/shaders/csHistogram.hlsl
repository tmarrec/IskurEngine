// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"

ConstantBuffer<HistogramConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=6, b0)"

[RootSignature(ROOT_SIG)]
[NumThreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint3 gtid: SV_GroupThreadID)
{
    Texture2D<float4> hdrTexture = ResourceDescriptorHeap[Constants.hdrTextureIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    uint width, height;
    hdrTexture.GetDimensions(width, height);

    uint2 pixel = tid.xy;
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }

    float d = depthTex.Load(int3(pixel, 0));
    if (d <= 1e-6f)
    {
        return;
    }

    float3 hdr = hdrTexture.Load(int3(pixel, 0)).rgb;
    float lum = dot(hdr, float3(0.2126f, 0.7152f, 0.0722f));
    float logL = clamp(log2(max(lum, 1e-5f)), Constants.minLogLum, Constants.maxLogLum);

    float norm = (logL - Constants.minLogLum) / max(Constants.maxLogLum - Constants.minLogLum, 1e-6f);
    uint idx = (uint)floor(norm * (Constants.numBuckets - 1u) + 0.5f);
    if (idx >= Constants.numBuckets)
    {
        idx = Constants.numBuckets - 1u;
    }

    RWStructuredBuffer<uint> histogramBuffer = ResourceDescriptorHeap[Constants.histogramBufferIndex];
    InterlockedAdd(histogramBuffer[idx], 1u);
}
