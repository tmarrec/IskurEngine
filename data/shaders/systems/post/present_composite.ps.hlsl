// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<PresentCompositeConstants> Constants : register(b0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=3, b0)"

[RootSignature(ROOT_SIG)]
float4 main(VSOut input) : SV_TARGET
{
    Texture2D<float4> hudlessTexture = ResourceDescriptorHeap[Constants.hudlessTextureIndex];
    Texture2D<float4> uiTexture = ResourceDescriptorHeap[Constants.uiTextureIndex];
    SamplerState linearSampler = SamplerDescriptorHeap[Constants.samplerIndex];

    float4 hudless = hudlessTexture.SampleLevel(linearSampler, input.uv, 0);
    float4 ui = uiTexture.SampleLevel(linearSampler, input.uv, 0);

    float3 composed = ui.rgb + (1.0 - ui.a) * hudless.rgb;
    return float4(composed, 1.0);
}
