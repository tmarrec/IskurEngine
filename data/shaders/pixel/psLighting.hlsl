// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define PI 3.14159265358979323846

#include "CPUGPU.h"
#include "utils/ReconstructWorldPos.hlsli"
#include "utils/EncodeDecodeNormal.hlsli"

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
    float metalness;
    float roughness;
    float3 N;
    float3 V;
    float3 L;
    float3 Lh;
    float3 Lr;
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
    float alpha = roughness * roughness;
    float alphaSq = alpha * alpha;
    float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * denom * denom);
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

// rotate vector v around the +Y axis by sunAzimuth:
float3 RotateEnv(float3 v)
{
    float azimuthOffsetted = C.sunAzimuth + 2.18166161; // 2.18166161 = 125.0 degrees in radians
    float c = cos(azimuthOffsetted);
    float s = sin(azimuthOffsetted);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

ShadingParams Gather(uint2 px, float3 world)
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[C.albedoTextureIndex];
    Texture2D<float2> normalTex = ResourceDescriptorHeap[C.normalTextureIndex];
    Texture2D<float2> materialTex = ResourceDescriptorHeap[C.materialTextureIndex];

    ShadingParams sp;

    // Base
    sp.V = normalize(C.cameraPos - world);
    sp.N = normalize(DecodeNormal(normalTex.Load(int3(px, 0))));
    sp.albedo = albedoTex.Load(int3(px, 0)).rgb;
    float2 mp = materialTex.Load(int3(px, 0));
    sp.metalness = saturate(mp.x);
    sp.roughness = saturate(mp.y);

    // Sun
    sp.L = normalize(-C.sunDir);
    sp.cosV = max(0.0, dot(sp.N, sp.V));
    sp.Lh = normalize(sp.L + sp.V);
    sp.Lr = reflect(-sp.V, sp.N);
    sp.cosL = max(0.0, dot(sp.N, sp.L));
    sp.cosLh = max(0.0, dot(sp.N, sp.Lh));
    sp.F0 = lerp(float3(0.04, 0.04, 0.04), sp.albedo, sp.metalness);

    return sp;
}

// RT Shadows visibility
float ComputeVis(uint2 px)
{
    float vis = 1.0;
    if (C.RTShadowsEnabled)
    {
        Texture2D<half> shadowRT = ResourceDescriptorHeap[C.raytracingOutputIndex];
        float occl = shadowRT.Load(int3(px, 0.0));
        vis = clamp(1.0 - occl, 0.0, 1.0);
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

    float3 diffuse = kd * sp.albedo / PI;
    float3 specular = (F * D * G) / max(1e-5, 4.0 * sp.cosL * sp.cosV);

    float3 sunColor = float3(1.0, 0.98, 0.92);
    float3 sunRadiance = sunColor * C.sunIntensity;
    return (diffuse + specular) * sp.cosL * sunRadiance;
}

float3 ComputeIBLSpecular(ShadingParams sp, float vis, SamplerState samp)
{
    TextureCube<float4> specularIBL = ResourceDescriptorHeap[C.specularIBLIndex];
    Texture2D<half2> brdfLUT = ResourceDescriptorHeap[C.brdfLUTIndex];

    uint w, h, specLevels;
    specularIBL.GetDimensions(0, w, h, specLevels);

    float rough = saturate(sp.roughness);
    float lodMax = max(0.0, (float)specLevels - 1.0);
    float lod = rough * lodMax;

    float3 pre = specularIBL.SampleLevel(samp, RotateEnv(sp.Lr), lod).rgb;
    float NoV  = saturate(sp.cosV);
    float2 brdf = brdfLUT.SampleLevel(samp, float2(NoV, 1.0 - rough), 0).rg;

    // Split-sum specular: prefiltered env * (F0 * brdf.x + brdf.y)
    float3 spec = (sp.F0 * brdf.x + brdf.y) * pre * C.IBLSpecularIntensity;

    // Shadow-strength remap (specular only)
    float ss = lerp(1.0, vis, 0.9); // 90% max for now, todo find a clean way to do that
    return spec * ss;
}

float SpecOcclusion(float ao, float NoV, float rough)
{
    // Filament-style approximation
    return saturate(pow(NoV + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao);
}

[RootSignature(ROOT_SIG)]
float4 main(VSOut input) : SV_TARGET
{
    uint2 px = min(uint2(input.position.xy), C.renderSize - 1);
    float2 uv = (float2(px) + 0.5) / float2(C.renderSize);
    Texture2D<float> depthTex = ResourceDescriptorHeap[C.depthTextureIndex];
    SamplerState samp = SamplerDescriptorHeap[C.samplerIndex];

    // Sky
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
        float3 skyDir = RotateEnv(-V);
        TextureCube<float4> envMap = ResourceDescriptorHeap[C.envMapIndex];
        return envMap.Sample(samp, skyDir) * C.skyIntensity;
    }

    float vis = ComputeVis(px);
    ShadingParams sp = Gather(px, world);
    float3 direct = ComputeDirect(sp) * clamp(vis, 0.0, 1.0);

    // GBuffer AO
    Texture2D<float> aoTex = ResourceDescriptorHeap[C.aoTextureIndex];
    float ao = aoTex.Load(int3(px, 0));

    // Indirect Diffuse
    Texture2D<float4> indirectDiffuseTexture = ResourceDescriptorHeap[C.indirectDiffuseTextureIndex];
    float3 indirectRaw = indirectDiffuseTexture.Load(int3(px, 0)).rgb;
    //return float4(indirectRaw, 1); // debug radiance

    float3 kd = lerp(1.0 - sp.F0, 0.0, sp.metalness);
    float3 indirect = kd * sp.albedo * indirectRaw;
    indirect *= ao;

    float NoV = saturate(sp.cosV);
    float3 iblSpec = ComputeIBLSpecular(sp, vis, samp);
    iblSpec *= SpecOcclusion(ao, NoV, sp.roughness);

    float3 light = direct + indirect + iblSpec;

    //return float4(sp.N * 0.5 + 0.5, 1.0); // debug normal

    return float4(light, 1);
}