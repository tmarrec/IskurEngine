// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/geometry/normal.hlsli"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"

ConstantBuffer<RTShadowsBlurConstants> Constants : register(b0);

static const float fadeStart = 5.f;
static const float fadeEnd = 50.f;
static const float planeDepthThreshold = 0.01f;
static const float normalDotMin = 0.75f;

float LinearizeDepth(float d, float zNear)
{
    // Reversed-Z with an effectively infinite far plane.
    return zNear / max(d, 1e-6f);
}

float LoadLinearDepth(Texture2D<float> depth, int2 uv, float zNear, out bool valid)
{
    float raw = depth.Load(int3(uv, 0));
    valid = raw > 1e-6f;
    return valid ? LinearizeDepth(raw, zNear) : 0.0f;
}

float EstimateAxisDepthGradient(Texture2D<float> depth, int2 uv, int2 axis, int2 extent, float centerDepth)
{
    bool validMinus = false;
    bool validPlus = false;
    const int2 minUv = int2(0, 0);
    const int2 maxUv = extent - 1;

    float depthMinus = LoadLinearDepth(depth, clamp(uv - axis, minUv, maxUv), Constants.zNear, validMinus);
    float depthPlus = LoadLinearDepth(depth, clamp(uv + axis, minUv, maxUv), Constants.zNear, validPlus);

    if (validMinus && validPlus)
    {
        return 0.5f * (depthPlus - depthMinus);
    }

    if (validPlus)
    {
        return depthPlus - centerDepth;
    }

    if (validMinus)
    {
        return centerDepth - depthMinus;
    }

    return 0.0f;
}

float ComputeSampleWeight(Texture2D<float> depth, Texture2D<float2> normalGeo, int2 sampleUv, float centerDepth, float3 centerNormal, float depthGradient, float sampleOffset)
{
    bool depthValid = false;
    float sampleDepth = LoadLinearDepth(depth, sampleUv, Constants.zNear, depthValid);
    if (!depthValid)
    {
        return 0.0f;
    }

    float expectedDepth = centerDepth + depthGradient * sampleOffset;
    float depthWeight = saturate(1.0f - abs(sampleDepth - expectedDepth) / planeDepthThreshold);
    if (depthWeight <= 0.0f)
    {
        return 0.0f;
    }

    float3 sampleNormal = DecodeNormal(normalGeo.Load(int3(sampleUv, 0)));
    float normalWeight = smoothstep(normalDotMin, 1.0f, saturate(dot(sampleNormal, centerNormal)));
    return depthWeight * normalWeight;
}

void Blur(int2 uv, int2 axis)
{
    Texture2D<half> input = ResourceDescriptorHeap[Constants.inputTextureIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[Constants.depthTextureIndex];
    Texture2D<float2> normalGeo = ResourceDescriptorHeap[Constants.normalGeoTextureIndex];
    RWTexture2D<half> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    uint width, height;
    input.GetDimensions(width, height);
    if (uv.x >= width || uv.y >= height)
    {
        return;
    }

    bool centerDepthValid = false;
    float cd = LoadLinearDepth(depth, uv, Constants.zNear, centerDepthValid);
    if (!centerDepthValid)
    {
        output[uv] = input.Load(int3(uv, 0));
        return;
    }

    float3 centerNormal = DecodeNormal(normalGeo.Load(int3(uv, 0)));
    float t = saturate((cd - fadeStart) / (fadeEnd - fadeStart));
    float depthGradient = EstimateAxisDepthGradient(depth, uv, axis, int2(width, height), cd);

    // Fixed 5-tap receiver-plane blur, fading to identity with distance.
    const float wFar = lerp(1.0 / 16.0, 0.0, t);
    const float wNear = lerp(4.0 / 16.0, 0.0, t);
    const float wCenter = lerp(6.0 / 16.0, 1.0, t);

    float sum = 0;
    float wsum = 0;
    const float offsets[5] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
    const float weights[5] = {wFar, wNear, wCenter, wNear, wFar};

    [unroll]
    for (uint i = 0; i < 5; ++i)
    {
        float offset = offsets[i];
        float kernelWeight = weights[i];
        if (offset == 0.0f)
        {
            sum += kernelWeight * input.Load(int3(uv, 0));
            wsum += kernelWeight;
            continue;
        }

        int2 sampleUv = clamp(uv + axis * int(offset), int2(0, 0), int2(width - 1, height - 1));
        float sampleWeight = ComputeSampleWeight(depth, normalGeo, sampleUv, cd, centerNormal, depthGradient, offset);
        if (sampleWeight > 0.0f)
        {
            float finalWeight = kernelWeight * sampleWeight;
            sum += finalWeight * input.Load(int3(sampleUv, 0));
            wsum += finalWeight;
        }
    }

    output[uv] = half((wsum > 0) ? sum / wsum : input.Load(int3(uv, 0)));
}

[RootSignature(ROOT_SIG)]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Blur(tid.xy, int2(Constants.axis));
}
