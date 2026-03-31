// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<BloomUpsampleConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), RootConstants(num32BitConstants=4, b0)"

[RootSignature(ROOT_SIG)]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Texture2D<float4> baseTexture = ResourceDescriptorHeap[Constants.baseTextureIndex];
    Texture2D<float4> bloomTexture = ResourceDescriptorHeap[Constants.bloomTextureIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.outputTextureIndex];
    SamplerState linearSampler = SamplerDescriptorHeap[Constants.samplerIndex];

    uint width, height;
    output.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }

    uint bloomWidth, bloomHeight;
    bloomTexture.GetDimensions(bloomWidth, bloomHeight);

    float2 uv = (float2(tid.xy) + 0.5f) / float2(width, height);
    float2 texel = 1.0f / float2(bloomWidth, bloomHeight);

    float3 bloom = 0.0f;
    bloom += bloomTexture.SampleLevel(linearSampler, uv, 0.0f).rgb * (4.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2(-texel.x, 0.0f), 0.0f).rgb * (2.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2( texel.x, 0.0f), 0.0f).rgb * (2.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2(0.0f, -texel.y), 0.0f).rgb * (2.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2(0.0f,  texel.y), 0.0f).rgb * (2.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2(-texel.x, -texel.y), 0.0f).rgb * (1.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2( texel.x, -texel.y), 0.0f).rgb * (1.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2(-texel.x,  texel.y), 0.0f).rgb * (1.0f / 16.0f);
    bloom += bloomTexture.SampleLevel(linearSampler, uv + float2( texel.x,  texel.y), 0.0f).rgb * (1.0f / 16.0f);

    output[tid.xy] = float4(baseTexture.Load(int3(tid.xy, 0)).rgb + bloom, 1.0f);
}
