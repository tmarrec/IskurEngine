// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/geometry/normal.hlsli"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=4, b0)"

cbuffer C : register(b0)
{
    PathTraceUpsampleConstants g;
}

uint2 RepresentativeFullPixel(uint2 halfPx, uint2 fullDim)
{
    return min(halfPx * 2u + 1u, fullDim - 1u);
}

float ComputeDepthWeight(float centerDepth, float sampleDepth)
{
    float depthDiff = abs(centerDepth - sampleDepth);
    return rcp(1.0f + depthDiff * 2048.0f);
}

float ComputeNormalWeight(float3 centerN, float3 sampleN)
{
    float nd = saturate(dot(centerN, sampleN));
    float t = saturate((nd - 0.6f) / 0.4f);
    return t * t;
}

[RootSignature(ROOT_SIG)]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[g.outputTextureIndex];
    uint fullW, fullH;
    output.GetDimensions(fullW, fullH);

    uint2 fullPx = dtid.xy;
    if (fullPx.x >= fullW || fullPx.y >= fullH)
    {
        return;
    }

    Texture2D<float4> halfIndirect = ResourceDescriptorHeap[g.inputTextureIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[g.depthTextureIndex];
    Texture2D<float2> normalTex = ResourceDescriptorHeap[g.normalGeoTextureIndex];

    float centerDepth = depthTex.Load(int3(fullPx, 0)).r;
    if (centerDepth <= 1e-6f)
    {
        output[fullPx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 centerN = DecodeNormal(normalTex.Load(int3(fullPx, 0)));

    uint halfW, halfH;
    halfIndirect.GetDimensions(halfW, halfH);
    int2 halfMax = int2((int)halfW - 1, (int)halfH - 1);

    // The half-res GI trace samples the odd full-res pixel in each 2x2 block,
    // so reconstruct around that lattice instead of the default texel-scaled grid.
    float2 halfPos = (float2(fullPx) - 1.0f.xx) * 0.5f;
    int2 base = int2(floor(halfPos));
    float2 fracPart = frac(halfPos);

    float3 accum = 0.0f.xxx;
    float weightSum = 0.0f;

    [unroll]
    for (uint oy = 0; oy < 2; ++oy)
    {
        float wy = (oy == 0u) ? (1.0f - fracPart.y) : fracPart.y;

        [unroll]
        for (uint ox = 0; ox < 2; ++ox)
        {
            float wx = (ox == 0u) ? (1.0f - fracPart.x) : fracPart.x;
            float bilinearWeight = wx * wy;
            if (bilinearWeight <= 0.0f)
            {
                continue;
            }

            int2 q = clamp(base + int2((int)ox, (int)oy), int2(0, 0), halfMax);
            uint2 sourceFullPx = RepresentativeFullPixel(uint2(q), uint2(fullW, fullH));
            float sampleDepth = depthTex.Load(int3(sourceFullPx, 0)).r;
            float3 sampleN = DecodeNormal(normalTex.Load(int3(sourceFullPx, 0)));

            float w = bilinearWeight;
            w *= ComputeDepthWeight(centerDepth, sampleDepth);
            w *= ComputeNormalWeight(centerN, sampleN);
            if (w <= 0.0f)
            {
                continue;
            }

            accum += halfIndirect.Load(int3(q, 0)).rgb * w;
            weightSum += w;
        }
    }

    if (weightSum <= 1e-5f)
    {
        uint2 nearestHalfPx = min(fullPx >> 1, uint2(halfW - 1u, halfH - 1u));
        accum = halfIndirect.Load(int3(nearestHalfPx, 0)).rgb;
    }
    else
    {
        accum *= rcp(weightSum);
    }

    output[fullPx] = float4(accum, 1.0f);
}
