// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/common.hlsli"

float Hash(float n)
{
    return frac(sin(n) * 43758.5453123);
}

float DistributionGGX(float NdotH, float a2)
{
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}
  
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 ToneMapACES(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 LinearToSRGB(float3 linearColor)
{
    float3 sRGBColor;
    sRGBColor.r = (linearColor.r <= 0.0031308) ? (12.92 * linearColor.r) : (1.055 * pow(linearColor.r, 1.0 / 2.4) - 0.055);
    sRGBColor.g = (linearColor.g <= 0.0031308) ? (12.92 * linearColor.g) : (1.055 * pow(linearColor.g, 1.0 / 2.4) - 0.055);
    sRGBColor.b = (linearColor.b <= 0.0031308) ? (12.92 * linearColor.b) : (1.055 * pow(linearColor.b, 1.0 / 2.4) - 0.055);
    return sRGBColor;
}

float4 main(VertexOut input) : SV_TARGET
{
    StructuredBuffer<Material> materialsBuffer = ResourceDescriptorHeap[PrimitiveConstants.materialsBufferIndex];
    Material material = materialsBuffer[PrimitiveConstants.materialIdx];

    // Color
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSampler = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= baseColorTexture.Sample(baseColorSampler, input.texCoord.xy);
    }

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
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[material.normalTextureIndex];
        SamplerState normalSampler = SamplerDescriptorHeap[material.normalSamplerIndex];
        float3 tangentNormal = normalTexture.Sample(normalSampler, input.texCoord.xy).rgb * 2.0 - 1.0;
        tangentNormal.xy *= material.normalScale;
        N = normalize(mul(tangentNormal, input.TBN));
    }
    
    // PBR
    float3 V = normalize(Globals.cameraPos - input.posWorld);

    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, baseColor.rgb, metallic);

    float3 Lo = float3(0, 0, 0);

    float3 L = normalize(Globals.sunDir);
    float3 H = normalize(V + L);
    float3 radiance = float3(1, 1, 1);

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float HdotV = saturate(dot(H, V));

    float NDF = DistributionGGX(NdotH, roughness * roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 kS = F;
    float3 kD = float3(1.0, 0.98, 0.95) - kS;
    kD *= 1.0 - metallic;

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;

    Lo += (kD * baseColor.rgb / PI + specular) * radiance * NdotL;

    float3 ambient = float(0.015).xxx * baseColor.rgb;

#ifdef ENABLE_RAYTRACED_SHADOWS
    Texture2D<half> ShadowRenderTarget = ResourceDescriptorHeap[Globals.raytracingOutputIndex];
    float2 uv = input.posWorldViewProj.xy;
    float shadowFactor = 1.0 - ShadowRenderTarget[uv];
#else
    float shadowFactor = 1;
#endif
    float3 color = ambient + (Lo * shadowFactor);
    
    float3 toneMapped = ToneMapACES(color);
    float3 sRGBColor = LinearToSRGB(toneMapped);

    // DEBUG DRAW NORMALS
    //sRGBColor = N * 0.5 + 0.5;

    /*
    if (input.posWorldViewProj.y / 1440 < input.posWorldViewProj.x / 2560)
    {
        sRGBColor = float3(Hash(input.meshletIndex), Hash(input.meshletIndex + 1.0), Hash(input.meshletIndex + 2.0));
    }
    */

    // DEBUG DRAW MESHLET
    //sRGBColor = float3(Hash(input.meshletIndex), Hash(input.meshletIndex + 1.0), Hash(input.meshletIndex + 2.0));
    
    //sRGBColor = input.posWorld;

    return float4(sRGBColor, 1);
}