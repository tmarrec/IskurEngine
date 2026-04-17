// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"
#include "include/core/hash.hlsli"
#include "include/geometry/normal.hlsli"

ConstantBuffer<PrimitiveConstants> Constants : register(b0);
ConstantBuffer<VertexConstants> VertexConstants : register(b1);

struct PSOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float2 normalGeo : SV_Target2;
    float material : SV_Target3;
    float2 motion : SV_Target4;
    float ao : SV_Target5;
    float3 emissive : SV_Target6;
};

float HashToUnit(uint x)
{
    return float(HashUint(x) & 0x00ffffffu) * (1.0f / 16777215.0f);
}

float3 HsvToRgb(float3 hsv)
{
    float3 rgb = saturate(abs(frac(hsv.x + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
    return hsv.z * lerp(1.0f.xxx, rgb, hsv.y);
}

float3 HashColor(uint seed)
{
    float hue = HashToUnit(seed ^ 0x68bc21ebu);
    return HsvToRgb(float3(hue, 1.0f, 1.0f));
}

PSOut main(VertexOut input, bool isFrontFace : SV_IsFrontFace)
{
    StructuredBuffer<Material> materialsBuffer = ResourceDescriptorHeap[Constants.materialsBufferIndex];
    Material material = materialsBuffer[Constants.materialIdx];

    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSampler = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= baseColorTexture.SampleBias(baseColorSampler, input.texCoord.xy, VertexConstants.materialTextureMipBias);
    }
    baseColor *= input.color;
    
    if (Constants.debugMeshletColorEnabled != 0)
    {
        baseColor.rgb = HashColor(input.meshletIndex);
    }

#ifdef ENABLE_ALPHA_TEST
    float alpha = baseColor.a;
    if (alpha < material.alphaCutoff)
    {
        discard;
    }
#endif // ENABLE_ALPHA_TEST

    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if (material.metallicRoughnessTextureIndex != -1)
    {
        Texture2D<float4> metallicRoughnessTexture = ResourceDescriptorHeap[material.metallicRoughnessTextureIndex];
        SamplerState metallicRoughnessSampler = SamplerDescriptorHeap[material.metallicRoughnessSamplerIndex];
        float4 textureValue = metallicRoughnessTexture.SampleBias(metallicRoughnessSampler, input.texCoord.xy, VertexConstants.materialTextureMipBias);
        metallic *= textureValue.b;
        roughness *= textureValue.g;
    }

    float3 geoN = normalize(input.N);
    const bool flipBackFaceForDoubleSided = (material.doubleSided != 0 && !isFrontFace);
    float3 N = geoN;
    if (material.normalTextureIndex != -1)
    {
        Texture2D<float2> normalTex = ResourceDescriptorHeap[material.normalTextureIndex];
        SamplerState normalSmp = SamplerDescriptorHeap[material.normalSamplerIndex];

        float2 rg = normalTex.SampleBias(normalSmp, input.texCoord.xy, VertexConstants.materialTextureMipBias).rg * 2.0f - 1.0f;
        rg *= material.normalScale;
        float z = sqrt(saturate(1.0f - dot(rg, rg)));
        float3 nTS = float3(rg, z);

        float3 Ng = geoN;
        float3 T = normalize(input.T);
        T = normalize(T - Ng * dot(Ng, T));
        float3 B = normalize(cross(Ng, T));

        float twSign = (input.Tw < 0.0f) ? -1.0f : 1.0f;
        B *= twSign;

        if (flipBackFaceForDoubleSided)
        {
            Ng = -Ng;
            T = -T;
            B = -B;
        }

        float3x3 TBN = float3x3(T, B, Ng);
        N = normalize(mul(nTS, TBN));
    }
    else if (flipBackFaceForDoubleSided)
    {
        N = -N;
    }

    if (flipBackFaceForDoubleSided)
    {
        geoN = -geoN;
    }

    float ao = 1.f;
    if (material.aoTextureIndex != -1)
    {
        Texture2D<float2> aoTex = ResourceDescriptorHeap[material.aoTextureIndex];
        SamplerState aoSmp = SamplerDescriptorHeap[material.aoSamplerIndex];
        ao = aoTex.SampleBias(aoSmp, input.texCoord.xy, VertexConstants.materialTextureMipBias).r;
    }

    float3 emissive = max(material.emissiveFactor, 0.0.xxx);
    if (material.emissiveTextureIndex != -1)
    {
        Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[material.emissiveTextureIndex];
        SamplerState emissiveSampler = SamplerDescriptorHeap[material.emissiveSamplerIndex];
        emissive *= emissiveTexture.SampleBias(emissiveSampler, input.texCoord.xy, VertexConstants.materialTextureMipBias).rgb;
    }
    
    PSOut output;
    output.albedo = float4(baseColor.rgb, baseColor.a);
    output.normal = float4(N, saturate(roughness));
    output.normalGeo = EncodeNormal(geoN);
    output.material = saturate(metallic);
    // DLSS expects backward-looking motion vectors that map the current pixel
    // to its previous-frame position, in normalized screen-space.
    float2 mv = (input.prevClipPosNoJ.xy / input.prevClipPosNoJ.z) - (input.currentClipPosNoJ.xy / input.currentClipPosNoJ.z);
    mv *= float2(0.5f, -0.5f);
    output.motion = mv;
    output.ao = ao;
    output.emissive = emissive;
    return output;
}

