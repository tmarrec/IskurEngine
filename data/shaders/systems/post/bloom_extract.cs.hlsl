// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<BloomDownsampleConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), RootConstants(num32BitConstants=6, b0)"

float3 Prefilter(float3 color)
{
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(Constants.threshold * Constants.softKnee, 1e-5f);
    float soft = clamp(brightness - Constants.threshold + knee, 0.0f, 2.0f * knee);
    soft = (soft * soft) / (4.0f * knee + 1e-5f);

    float contribution = max(brightness - Constants.threshold, soft);
    contribution /= max(brightness, 1e-5f);
    return color * contribution;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float KarisWeight(float3 color)
{
    return rcp(1.0f + Luminance(color));
}

[RootSignature(ROOT_SIG)]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Texture2D<float4> input = ResourceDescriptorHeap[Constants.inputTextureIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.outputTextureIndex];
    SamplerState linearSampler = SamplerDescriptorHeap[Constants.samplerIndex];

    uint outWidth, outHeight;
    output.GetDimensions(outWidth, outHeight);
    if (tid.x >= outWidth || tid.y >= outHeight)
    {
        return;
    }

    uint inWidth, inHeight;
    input.GetDimensions(inWidth, inHeight);
    float2 uv = (float2(tid.xy) + 0.5f) / float2(outWidth, outHeight);
    float2 texel = 1.0f / float2(inWidth, inHeight);
    const float2 offsets[13] =
    {
        float2(-1.0f, -1.0f),
        float2( 0.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-0.5f, -0.5f),
        float2( 0.5f, -0.5f),
        float2(-1.0f,  0.0f),
        float2( 0.0f,  0.0f),
        float2( 1.0f,  0.0f),
        float2(-0.5f,  0.5f),
        float2( 0.5f,  0.5f),
        float2(-1.0f,  1.0f),
        float2( 0.0f,  1.0f),
        float2( 1.0f,  1.0f)
    };

    const float weights[13] =
    {
        1.0f / 32.0f,
        1.0f / 16.0f,
        1.0f / 32.0f,
        1.0f / 8.0f,
        1.0f / 8.0f,
        1.0f / 16.0f,
        1.0f / 8.0f,
        1.0f / 16.0f,
        1.0f / 8.0f,
        1.0f / 8.0f,
        1.0f / 32.0f,
        1.0f / 16.0f,
        1.0f / 32.0f
    };

    float3 sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (uint i = 0; i < 13; ++i)
    {
        float3 sampleColor = input.SampleLevel(linearSampler, uv + offsets[i] * texel, 0.0f).rgb;
        float sampleWeight = weights[i];
        if (Constants.applyThreshold != 0u)
        {
            sampleColor = Prefilter(sampleColor);
            sampleWeight *= KarisWeight(sampleColor);
        }

        sum += sampleColor * sampleWeight;
        weightSum += sampleWeight;
    }

    output[tid.xy] = float4((weightSum > 0.0f) ? (sum / weightSum) : 0.0f.xxx, 1.0f);
}
