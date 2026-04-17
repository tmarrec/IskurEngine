// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/geometry/reconstruct_world_pos.hlsli"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=25, b0)"

ConstantBuffer<DLSSRRGuideConstants> Constants : register(b0);

float3 EnvBRDFApprox2(float3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);

    const float4 X = float4(1.0f, NoV, NoV * NoV, NoV * NoV * NoV);
    const float4 Y = float4(1.0f, alpha, alpha * alpha, alpha * alpha * alpha);

    const float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    const float3x3 M2 = float3x3(1.0f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    const float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    const float3x3 M4 = float3x3(1.0f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);

    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specularColor.g * 50.0f);

    return specularColor * max(0.0f, scale) + max(0.0f, bias);
}

[RootSignature(ROOT_SIG)]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    RWTexture2D<float4> diffuseAlbedoOut = ResourceDescriptorHeap[Constants.diffuseAlbedoOutputTextureIndex];
    RWTexture2D<float4> specularAlbedoOut = ResourceDescriptorHeap[Constants.specularAlbedoOutputTextureIndex];

    uint width, height;
    diffuseAlbedoOut.GetDimensions(width, height);

    const uint2 px = dtid.xy;
    if (px.x >= width || px.y >= height)
    {
        return;
    }

    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    const float depth = depthTex.Load(int3(px, 0)).r;
    if (depth <= 1e-6f)
    {
        diffuseAlbedoOut[px] = 0.0f.xxxx;
        specularAlbedoOut[px] = 0.0f.xxxx;
        return;
    }

    Texture2D<float4> albedoTex = ResourceDescriptorHeap[Constants.albedoTextureIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[Constants.normalTextureIndex];
    Texture2D<float> materialTex = ResourceDescriptorHeap[Constants.materialTextureIndex];

    const float3 baseColor = max(albedoTex.Load(int3(px, 0)).rgb, 0.0f.xxx);
    const float4 normalRoughness = normalTex.Load(int3(px, 0));
    const float metallic = saturate(materialTex.Load(int3(px, 0)));
    const float roughness = saturate(normalRoughness.w);
    const float3 N = normalize(normalRoughness.xyz);

    const float2 uv = (float2(px) + 0.5f) / float2(width, height);
    const float3 worldPos = ReconstructWorldPos(uv, depth, Constants.invViewProj);
    const float3 V = normalize(Constants.cameraPos - worldPos);
    const float NoV = saturate(dot(N, V));

    const float3 diffuseAlbedo = baseColor * (1.0f - metallic);
    const float3 specularColor = lerp(0.04f.xxx, baseColor, metallic);
    const float3 specularAlbedo = EnvBRDFApprox2(specularColor, roughness * roughness, NoV);

    diffuseAlbedoOut[px] = float4(diffuseAlbedo, 1.0f);
    specularAlbedoOut[px] = float4(max(specularAlbedo, 0.0f.xxx), 1.0f);
}
