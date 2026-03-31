// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<SkyMotionPassConstants> C : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  CBV(b0)"

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

[RootSignature(ROOT_SIG)]
float2 main(VSOut input) : SV_TARGET
{
    uint2 px = uint2(input.position.xy);
    Texture2D<float> depthTex = ResourceDescriptorHeap[C.depthTextureIndex];
    float depth = depthTex.Load(int3(px, 0));
    if (depth > 1e-6f)
    {
        discard;
    }

    // Convert UV to D3D NDC for a direction ray.
    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);

    float3 currentViewDir = normalize(float3(ndc.x / C.projectionNoJitter._11, ndc.y / C.projectionNoJitter._22, -1.0f));
    float3 worldDir = normalize(mul(float4(currentViewDir, 0.0f), C.invView).xyz);
    float3 prevViewDir = normalize(mul(float4(worldDir, 0.0f), C.prevView).xyz);

    float4 currentClip = mul(float4(currentViewDir, 0.0f), C.projectionNoJitter);
    float4 prevClip = mul(float4(prevViewDir, 0.0f), C.prevProjectionNoJitter);

    float2 currentNdc = currentClip.xy / max(abs(currentClip.w), 1e-6f);
    float2 prevNdc = prevClip.xy / max(abs(prevClip.w), 1e-6f);

    float2 mv = (prevNdc - currentNdc) * float2(0.5f, -0.5f);
    return mv;
}

