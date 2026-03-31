// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "include/core/math_constants.hlsli"
#include "CPUGPU.h"
#include "include/geometry/reconstruct_world_pos.hlsli"
#include "include/geometry/normal.hlsli"

ConstantBuffer<LightingPassConstants> C : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  CBV(b0)"

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct ShadingParams
{
    float3 albedo;
    float3 emissive;
    float metalness;
    float roughness;
    float3 N;
    float3 V;
    float3 L;
    float3 Lh;
    float cosL;
    float cosV;
    float cosLh;
    float3 F0;
};

float3 FresnelSchlick(float3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// GGX/Towbridge-Reitz normal distribution function
// Uses Disney's reparametrization of alpha = roughness^2
float NdfGGX(float cosLh, float roughness)
{
    float alpha = max(roughness * roughness, 1e-4f);
    float alphaSq = alpha * alpha;
    float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / max(IE_PI * denom * denom, 1e-7f);
}

// Single term for separable Schlick-GGX below
float GaSchlickG1(float cosTheta, float k)
{
    return cosTheta / (cosTheta * (1.0 - k) + k);
}

// Schlick-GGX approximation of geometric attenuation function using Smith's method
float GaSchlickGGX(float cosLi, float cosV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0; // Epic suggests using this roughness remapping for analytic lights
    return GaSchlickG1(cosLi, k) * GaSchlickG1(cosV, k);
}

float3 NormalizeSunTint(float3 sunColor)
{
    float3 tint = max(sunColor, 0.0.xxx);
    float luminance = dot(tint, float3(0.2126, 0.7152, 0.0722));
    return (luminance > 1e-4f) ? (tint / luminance) : 0.0.xxx;
}

ShadingParams Gather(uint2 px, float3 world)
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[C.albedoTextureIndex];
    Texture2D<float2> normalTex = ResourceDescriptorHeap[C.normalTextureIndex];
    Texture2D<float2> materialTex = ResourceDescriptorHeap[C.materialTextureIndex];
    Texture2D<float4> emissiveTex = ResourceDescriptorHeap[C.emissiveTextureIndex];

    ShadingParams sp;

    sp.V = normalize(C.cameraPos - world);
    sp.N = DecodeNormal(normalTex.Load(int3(px, 0)));
    sp.albedo = albedoTex.Load(int3(px, 0)).rgb;
    sp.emissive = emissiveTex.Load(int3(px, 0)).rgb;
    float2 mp = materialTex.Load(int3(px, 0));
    sp.metalness = saturate(mp.x);
    sp.roughness = saturate(mp.y);

    sp.L = normalize(-C.sunDir);
    sp.cosV = max(0.0, dot(sp.N, sp.V));
    sp.Lh = normalize(sp.L + sp.V);
    sp.cosL = max(0.0, dot(sp.N, sp.L));
    sp.cosLh = max(0.0, dot(sp.N, sp.Lh));
    sp.F0 = lerp(float3(0.04, 0.04, 0.04), sp.albedo, sp.metalness);

    return sp;
}

// RT Shadows visibility
float ComputeVis(uint2 px)
{
    if (C.debugMeshletColorEnabled != 0u)
    {
        return 1.0f;
    }

    float vis = 1.0;
    if (C.rtShadowsEnabled)
    {
        Texture2D<half> shadowRT = ResourceDescriptorHeap[C.rtShadowTextureIndex];
        float occl = shadowRT.Load(int3(px, 0.0));
        vis = clamp(1.0 - occl, 0.0, 1.0);
        vis = lerp(saturate(C.shadowMinVisibility), 1.0f, vis);
    }
    return vis;
}

float3 ComputeDirect(ShadingParams sp)
{
    float VoH = saturate(dot(sp.Lh, sp.V));
    float3 F = FresnelSchlick(sp.F0, VoH);
    float D = NdfGGX(sp.cosLh, sp.roughness);
    float G = GaSchlickGGX(sp.cosL, sp.cosV, sp.roughness);
    float3 kd = (1.0 - F) * (1.0 - sp.metalness);

    float3 diffuse = kd * sp.albedo / IE_PI;
    float3 specular = (F * D * G) / max(1e-5, 4.0 * sp.cosL * sp.cosV);

    float3 sunRadiance = NormalizeSunTint(C.sunColor) * C.sunIntensity;
    return (diffuse + specular) * sp.cosL * sunRadiance;
}

[RootSignature(ROOT_SIG)]
float4 main(VSOut input) : SV_TARGET
{
    uint2 px = min(uint2(input.position.xy), C.renderSize - 1);
    float2 uv = (float2(px) + 0.5) / float2(C.renderSize);
    Texture2D<float> depthTex = ResourceDescriptorHeap[C.depthTextureIndex];
    SamplerState samp = SamplerDescriptorHeap[C.samplerIndex];

    float depth = depthTex.Load(int3(px, 0));
    bool isSky = depth <= 1e-6f;
    if (isSky)
    {
        depth += 1e-6f; // Avoid division by zero in ReconstructWorldPos
    }
    float3 world = ReconstructWorldPos(uv, depth, C.invViewProj);
    float3 V = normalize(C.cameraPos - world);
    if (isSky)
    {
        TextureCube<float4> skyCube = ResourceDescriptorHeap[C.skyCubeIndex];
        float3 skyColor = skyCube.SampleLevel(samp, -V, 0.0f).rgb;
        return float4(skyColor * C.skyIntensity, 1.0);
    }

    float vis = ComputeVis(px);
    ShadingParams sp = Gather(px, world);
    if (C.debugMeshletColorEnabled != 0u)
    {
        return float4(sp.albedo, 1.0f);
    }
    if (C.debugViewMode == 2u)
    {
        return float4(sp.N * 0.5f + 0.5f, 1.0f);
    }
    float3 direct = ComputeDirect(sp) * clamp(vis, 0.0, 1.0);

    Texture2D<float> aoTex = ResourceDescriptorHeap[C.aoTextureIndex];
    float ao = aoTex.Load(int3(px, 0));
    float aoDiffuse = lerp(1.0, ao, 0.6);

    Texture2D<float4> indirectDiffuseTexture = ResourceDescriptorHeap[C.indirectDiffuseTextureIndex];
    float3 indirectRaw = indirectDiffuseTexture.Load(int3(px, 0)).rgb;
    if (C.debugViewMode == 1u)
    {
        return float4(indirectRaw, 1.0f);
    }

    float3 kd = lerp(1.0 - sp.F0, 0.0, sp.metalness);
    // Apply moderate AO to RT indirect to avoid overly bright shadowed interiors.
    float indirectAO = lerp(1.0, aoDiffuse, 0.7);
    float3 indirect = kd * sp.albedo * indirectRaw * indirectAO;
    indirect *= max(C.rtIndirectDiffuseStrength, 0.0f);

    float NoV = saturate(sp.cosV);
    float3 finalSpec = 0.0f.xxx;
    if (C.rtSpecularEnabled != 0)
    {
        Texture2D<float4> rtSpecularTexture = ResourceDescriptorHeap[C.rtSpecularTextureIndex];
        float3 rtSpec = rtSpecularTexture.Load(int3(px, 0)).rgb;

        // Match the same dielectric-vs-metal behavior for RT specular.
        const float dielectricSpecScale = 0.0f;
        float specMaterialScale = lerp(dielectricSpecScale, 1.0f, sp.metalness);
        rtSpec *= specMaterialScale;

        // Shape RT reflected radiance by Fresnel to avoid over-bright broad reflections.
        rtSpec *= FresnelSchlick(sp.F0, NoV);

        // Keep some shadow influence to avoid overly bright reflections in deep shadow.
        if (C.debugMeshletColorEnabled == 0u)
        {
            float rtShadowInfluence = lerp(0.30f, 0.60f, saturate(1.0f - sp.roughness));
            rtSpec *= lerp(1.0f, saturate(vis), rtShadowInfluence);
        }

        float rtWeight = saturate(1.0f - sp.roughness);
        rtWeight *= rtWeight; // smoother materials favor RT specular more aggressively
        rtWeight *= saturate(C.rtSpecularStrength);

        finalSpec = rtSpec * rtWeight;
    }

    float3 ambient = sp.albedo * C.ambientStrength;
    ambient *= lerp(1.0f, aoDiffuse, 0.3f);
    float3 light = direct + indirect + finalSpec + ambient + sp.emissive;

    return float4(light, 1);
}

