// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/Common.hlsli"
#include "data/CPUGPU.h"
#include "data/shaders/utils/EncodeDecodeNormal.hlsli"

ConstantBuffer<PrimitiveConstants> Constants : register(b0);

struct PSOut
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float2 material : SV_Target2;
    float2 motion : SV_Target3;
    float ao : SV_Target4;
};

PSOut main(VertexOut input, bool isFrontFace : SV_IsFrontFace)
{
    StructuredBuffer<Material> materialsBuffer = ResourceDescriptorHeap[Constants.materialsBufferIndex];
    Material material = materialsBuffer[Constants.materialIdx];

    // Color
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSampler = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= baseColorTexture.Sample(baseColorSampler, input.texCoord.xy);
    }
    
    /*
    // Debug meshlet colors
    baseColor.rgb = 0.2 + 0.8 * frac(
        float3(0.1031, 0.11369, 0.13787) * float(input.meshletIndex) +
        float3(0.3,    0.6,     0.9)
    );

    uint h = input.meshletIndex * 1664525u + 1013904223u;
    uint r = h * 1664525u + 1013904223u;
    uint g = r * 1664525u + 1013904223u;
    uint b = g * 1664525u + 1013904223u;
    baseColor.rgb = 0.2 + 0.8 * (float3(r, g, b) * (1.0/4294967296.0));
    */

    // Alpha
#ifdef ENABLE_ALPHA_TEST
    float alpha = baseColor.a;
    if (alpha < material.alphaCutoff)
    {
        discard;
    }
#endif // ENABLE_ALPHA_TEST

    // Metallic Roughness
    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if (material.metallicRoughnessTextureIndex != -1)
    {
        Texture2D<float4> metallicRoughnessTexture = ResourceDescriptorHeap[material.metallicRoughnessTextureIndex];
        SamplerState metallicRoughnessSampler = SamplerDescriptorHeap[material.metallicRoughnessSamplerIndex];
        float4 textureValue = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.texCoord.xy);
        metallic *= textureValue.b;
        roughness *= textureValue.g;
    }

    // Normal
    float3 N = normalize(input.normal);
    if (material.normalTextureIndex != -1)
    {
        Texture2D<float2> normalTex = ResourceDescriptorHeap[material.normalTextureIndex];
        SamplerState normalSmp = SamplerDescriptorHeap[material.normalSamplerIndex];

        float2 rg = normalTex.Sample(normalSmp, input.texCoord.xy).rg * 2.0f - 1.0f;
        rg *= material.normalScale;
        float z = sqrt(saturate(1.0f - dot(rg, rg)));
        float3 nTS = float3(rg, z);

        float3 Ng = normalize(input.N);
        float3 T = normalize(input.T);
        T = normalize(T - Ng * dot(Ng, T));
        float3 B = normalize(cross(Ng, T));

        float twSign = (input.Tw < 0.0f) ? -1.0f : 1.0f;
        B *= twSign;

        if (material.doubleSided != 0 && !isFrontFace)
        {
            Ng = -Ng;
            T = -T;
            B = -B;
        }

        float3x3 TBN = float3x3(T, B, Ng);
        N = normalize(mul(nTS, TBN));
    }
    // Octahedral Normal Encoding
    float2 encodedNormal = EncodeNormal(N);

    // AO
    float ao = 1.f;
    if (material.aoTextureIndex != -1)
    {
        Texture2D<float2> aoTex = ResourceDescriptorHeap[material.aoTextureIndex];
        SamplerState aoSmp = SamplerDescriptorHeap[material.aoSamplerIndex];
        ao = aoTex.Sample(aoSmp, input.texCoord.xy).r;
    }
    
    PSOut output;
    output.albedo = float4(baseColor.rgb, baseColor.a);
    output.normal = encodedNormal;
    output.material = float2(metallic, roughness);
    float2 mv = (input.prevClipPosNoJ.xy / input.prevClipPosNoJ.w) - (input.currentClipPosNoJ.xy / input.currentClipPosNoJ.w);
    mv *= float2(0.5f, -0.5f); // FSR spec
    output.motion = mv;
    output.ao = ao;
    return output;
}