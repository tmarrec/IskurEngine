// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/geometry/reconstruct_world_pos.hlsli"
#include "include/rt/rt_shared.hlsli"

ConstantBuffer<PathTraceConstants> Constants : register(b0);

static const float RT_PRIMARY_BOUNCE_CONTINUATION_RAY_ORIGIN_BIAS = 0.025f;
static const float PATH_TRACE_DIRECT_SHADOW_RAY_TMIN = 1e-4f;
static const float PATH_TRACE_DIRECT_SHADOW_RAY_MIN_BIAS = 1e-3f;
static const float PATH_TRACE_DIRECT_SHADOW_RAY_VIEW_BIAS_SCALE = 3e-5f;
static const uint PATH_TRACE_RUSSIAN_ROULETTE_MIN_BOUNCE = 1u;

struct SurfaceState
{
    float3 pos;
    float3 geoN;
    float3 albedo;
    float3 emissive;
    float metalness;
    float roughness;
    float3 N;
    float3 V;
};

struct PathResult
{
    float3 radiance;
    float specHitDistance;
};

struct BsdfSample
{
    float3 wi;
    float3 f;
    float pdf;
    bool wasSpecular;
};

struct BsdfMixtureWeights
{
    float pDiffuse;
    float pSpecular;
};

struct SunLightData
{
    float3 axis;
    float cosThetaMax;
    float3 coneRadiance;
    float conePdf;
    bool visible;
};

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float3 FresnelSchlick(float3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * Pow5(1.0 - cosTheta);
}

float3 ComputeF0(float3 albedo, float metalness)
{
    return lerp(float3(0.04f, 0.04f, 0.04f), albedo, metalness);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float MaxComponent(float3 v)
{
    return max(v.r, max(v.g, v.b));
}

float ComputeRussianRouletteSurvivalProbability(float3 throughput)
{
    float survivalProbability = saturate(MaxComponent(throughput));
    return clamp(survivalProbability, 0.05f, 0.95f);
}

float NdfGGX(float cosLh, float roughness)
{
    float alpha = max(roughness * roughness, 1e-4f);
    float alphaSq = alpha * alpha;
    float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / max(IE_PI * denom * denom, 1e-7f);
}

float SmithLambdaGGX(float cosTheta, float roughness)
{
    float safeCosTheta = max(saturate(cosTheta), 1e-4f);
    float alpha = max(roughness * roughness, 1e-4f);
    float sinThetaSq = saturate(1.0f - safeCosTheta * safeCosTheta);
    float tanThetaSq = sinThetaSq / max(safeCosTheta * safeCosTheta, 1e-6f);
    return 0.5f * (sqrt(1.0f + alpha * alpha * tanThetaSq) - 1.0f);
}

float SmithG1GGX(float cosTheta, float roughness)
{
    return 1.0f / (1.0f + SmithLambdaGGX(cosTheta, roughness));
}

float SmithGGX(float cosLi, float cosV, float roughness)
{
    return SmithG1GGX(cosLi, roughness) * SmithG1GGX(cosV, roughness);
}

float ComputeShadowOriginBias(float3 hitPos, float3 geoN, float3 rayDir)
{
    float3 lightDir = normalize(rayDir);
    if (dot(geoN, lightDir) < 0.0f)
    {
        geoN = -geoN;
    }

    float viewDist = length(hitPos - Constants.cameraPos);
    float baseBias = max(PATH_TRACE_DIRECT_SHADOW_RAY_MIN_BIAS, PATH_TRACE_DIRECT_SHADOW_RAY_VIEW_BIAS_SCALE * viewDist);
    float slopeScale = 1.0f + 2.0f * (1.0f - saturate(dot(geoN, lightDir)));
    return baseBias * slopeScale;
}

float3 SampleSkyBackground(float3 dir)
{
    SamplerState envSamp = SamplerDescriptorHeap[Constants.samplerIndex];
    TextureCube<float4> skyCube = ResourceDescriptorHeap[Constants.skyCubeIndex];
    return skyCube.SampleLevel(envSamp, dir, 0.0f).rgb * Constants.skyIntensity;
}

SunLightData BuildSunLightData()
{
    SunLightData sun;
    sun.axis = normalize(-Constants.sunDir);
    sun.visible = Constants.sunIntensity > 0.0f && MaxComponent(Constants.sunColor) > 0.0f && sun.axis.y > 0.0f;
    sun.cosThetaMax = cos(radians(Constants.sunDiskAngleDeg));
    float solidAngle = IE_TWO_PI * max(1.0f - sun.cosThetaMax, 1e-6f);
    float3 sunRadiance = NormalizeSunTint(Constants.sunColor) * Constants.sunIntensity;
    sun.coneRadiance = sunRadiance / solidAngle;
    sun.conePdf = rcp(solidAngle);
    return sun;
}

float CosineHemispherePdf(float cosTheta)
{
    return saturate(cosTheta) * IE_INV_PI;
}

float PowerHeuristic(float pdfA, float pdfB)
{
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    return a2 / max(a2 + b2, 1e-6f);
}

float3 ToLocal(float3 dir, float3 T, float3 B, float3 N)
{
    return float3(dot(dir, T), dot(dir, B), dot(dir, N));
}

float3 ToWorld(float3 dir, float3 T, float3 B, float3 N)
{
    return T * dir.x + B * dir.y + N * dir.z;
}

float3 SampleGGXVNDFLocal(float3 V, float roughness, float2 u)
{
    float alpha = max(roughness * roughness, 1e-4f);

    float3 Vh = normalize(float3(alpha * V.x, alpha * V.y, V.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 0.0f) ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(u.x);
    float phi = IE_TWO_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = lerp(sqrt(saturate(1.0f - t1 * t1)), t2, s);

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0f - t1 * t1 - t2 * t2)) * Vh;
    float3 H = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0f)));
    return H;
}

float3 SampleGGXVNDFWorld(float3 V, float3 N, float roughness, float2 u)
{
    float3 T, B;
    BuildOrthonormalBasis(N, T, B);
    float3 Vlocal = normalize(ToLocal(V, T, B, N));
    float3 Hlocal = SampleGGXVNDFLocal(Vlocal, roughness, u);
    return normalize(ToWorld(Hlocal, T, B, N));
}

float3 EvaluateDiffuseBrdf(SurfaceState surface, float3 wi)
{
    float3 H = normalize(wi + surface.V);
    float3 F0 = ComputeF0(surface.albedo, surface.metalness);
    float VoH = saturate(dot(surface.V, H));
    float3 F = FresnelSchlick(F0, VoH);
    float3 kd = (1.0f - F) * (1.0f - surface.metalness);
    return kd * surface.albedo * IE_INV_PI;
}

float3 EvaluateSpecularBrdf(SurfaceState surface, float3 wi)
{
    float cosL = saturate(dot(surface.N, wi));
    float cosV = saturate(dot(surface.N, surface.V));
    if (cosL <= 0.0f || cosV <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float roughness = surface.roughness;
    float3 H = normalize(wi + surface.V);
    float cosLh = saturate(dot(surface.N, H));
    float3 F0 = ComputeF0(surface.albedo, surface.metalness);
    float VoH = saturate(dot(surface.V, H));
    if (VoH <= 0.0f || cosLh <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 F = FresnelSchlick(F0, VoH);
    float D = NdfGGX(cosLh, roughness);
    float G = SmithGGX(cosL, cosV, roughness);
    return (F * D * G) / max(1e-5f, 4.0f * cosL * cosV);
}

float SpecularReflectionPdf(SurfaceState surface, float3 wi)
{
    float cosL = saturate(dot(surface.N, wi));
    float cosV = saturate(dot(surface.N, surface.V));
    if (cosL <= 0.0f || cosV <= 0.0f)
    {
        return 0.0f;
    }

    float roughness = surface.roughness;
    float3 H = normalize(wi + surface.V);
    float cosLh = saturate(dot(surface.N, H));
    float VoH = saturate(dot(surface.V, H));
    if (cosLh <= 0.0f || VoH <= 0.0f)
    {
        return 0.0f;
    }

    float D = NdfGGX(cosLh, roughness);
    float G1 = SmithG1GGX(cosV, roughness);
    return (D * G1) / max(4.0f * cosV, 1e-6f);
}

BsdfMixtureWeights ComputeBsdfMixtureWeights(SurfaceState surface)
{
    BsdfMixtureWeights weights;
    weights.pDiffuse = 0.0f;
    weights.pSpecular = 0.0f;

    float diffuseWeight = Luminance(EvaluateDiffuseBrdf(surface, surface.N));
    float specularWeight = Luminance(EvaluateSpecularBrdf(surface, reflect(-surface.V, surface.N)));
    float totalWeight = diffuseWeight + specularWeight;
    if (totalWeight > 1e-5f)
    {
        weights.pDiffuse = diffuseWeight / totalWeight;
        weights.pSpecular = specularWeight / totalWeight;
    }

    // Keep a small diffuse probability on smooth non-metals so the mirror-peak
    // heuristic cannot starve diffuse transport completely at grazing angles.
    float smoothNonMetal = (1.0f - surface.metalness) * (1.0f - saturate(surface.roughness * 4.0f));
    float minDiffuse = 0.05f * smoothNonMetal;
    if (weights.pDiffuse < minDiffuse)
    {
        weights.pDiffuse = minDiffuse;
        weights.pSpecular = 1.0f - minDiffuse;
    }

    return weights;
}

float ComputePathEventPdf(SurfaceState surface, float3 wi, BsdfMixtureWeights weights)
{
    float pdf = 0.0f;

    if (weights.pDiffuse > 0.0f)
    {
        float NoL = saturate(dot(surface.N, wi));
        pdf += weights.pDiffuse * CosineHemispherePdf(NoL);
    }

    if (weights.pSpecular > 0.0f)
    {
        pdf += weights.pSpecular * SpecularReflectionPdf(surface, wi);
    }

    return pdf;
}

float3 SampleUniformConeWorld(float2 u, float3 axis, float cosThetaMax)
{
    float phi = IE_TWO_PI * u.x;
    float cosTheta = lerp(1.0f, cosThetaMax, u.y);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));

    float3 T, B;
    BuildOrthonormalBasis(axis, T, B);
    return normalize(T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + axis * cosTheta);
}

float3 SampleSunDirection(float2 u, SunLightData sun)
{
    return SampleUniformConeWorld(u, sun.axis, sun.cosThetaMax);
}

bool IsDirectionInsideSunCone(float3 dir, SunLightData sun)
{
    return sun.visible && dot(dir, sun.axis) >= sun.cosThetaMax;
}

SurfaceState GatherSurface(uint2 px, float3 worldPos, float3 geoNormal)
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[Constants.albedoTextureIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[Constants.normalTextureIndex];
    Texture2D<float> materialTex = ResourceDescriptorHeap[Constants.materialTextureIndex];
    Texture2D<float4> emissiveTex = ResourceDescriptorHeap[Constants.emissiveTextureIndex];

    SurfaceState surface;

    float4 normalRoughness = normalTex.Load(int3(px, 0));
    surface.pos = worldPos;
    surface.geoN = geoNormal;
    surface.N = normalize(normalRoughness.xyz);
    surface.albedo = albedoTex.Load(int3(px, 0)).rgb;
    surface.emissive = emissiveTex.Load(int3(px, 0)).rgb;
    surface.metalness = saturate(materialTex.Load(int3(px, 0)));
    surface.roughness = saturate(normalRoughness.w);
    surface.V = normalize(Constants.cameraPos - worldPos);
    return surface;
}

SurfaceState SurfaceFromPayload(RayPayload payload, float3 outgoingDir)
{
    SurfaceState surface;
    surface.pos = payload.hitPos;
    surface.geoN = payload.geoN;
    surface.albedo = payload.albedo;
    surface.emissive = payload.emissive;
    surface.metalness = payload.metalness;
    surface.roughness = payload.roughness;
    surface.N = payload.hitN;
    surface.V = outgoingDir;
    return surface;
}

float3 EvaluateSunDirect(RaytracingAccelerationStructure scene, SurfaceState surface, BsdfMixtureWeights weights, SunLightData sun, inout uint seed)
{
    if (!sun.visible)
    {
        return 0.0f.xxx;
    }

    if (weights.pDiffuse <= 0.0f && weights.pSpecular <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 L = SampleSunDirection(float2(Rand(seed), Rand(seed)), sun);
    float cosL = saturate(dot(surface.N, L));
    if (cosL <= 0.0f)
    {
        return 0.0.xxx;
    }

    float diffuseVisibility = 1.0f;
    float specularVisibility = 1.0f;
    if (Constants.rtShadowsEnabled != 0u)
    {
        float originBias = ComputeShadowOriginBias(surface.pos, surface.geoN, L);
        const bool use2StateOmmRays = (Constants.rtOmmsEnabled != 0u) && (Constants.rtUse2StateOmmRays != 0u);
        float rawVisibility = TraceVisibilityWithBias(scene, surface.pos, surface.geoN, L, originBias, PATH_TRACE_DIRECT_SHADOW_RAY_TMIN, use2StateOmmRays,
                                                      Constants.primInfoBufferIndex, Constants.materialsBufferIndex);
        rawVisibility = saturate(rawVisibility);
        diffuseVisibility = lerp(saturate(Constants.shadowMinVisibility), 1.0f, rawVisibility);
        specularVisibility = lerp(saturate(Constants.specularShadowMinVisibility), 1.0f, rawVisibility);
    }

    float3 diffuseBrdf = EvaluateDiffuseBrdf(surface, L);
    float3 specularBrdf = EvaluateSpecularBrdf(surface, L);
    float3 brdf = diffuseBrdf * diffuseVisibility + specularBrdf * specularVisibility;
    float bsdfPdf = ComputePathEventPdf(surface, L, weights);
    float lightPdf = sun.conePdf;
    float misWeight = PowerHeuristic(lightPdf, bsdfPdf);
    return brdf * cosL * sun.coneRadiance * (misWeight / max(lightPdf, 1e-6f));
}

bool SamplePathEvent(SurfaceState surface, BsdfMixtureWeights weights, inout uint seed, out BsdfSample sample)
{
    if (weights.pDiffuse <= 0.0f && weights.pSpecular <= 0.0f)
    {
        return false;
    }

    if (weights.pSpecular > 0.0f && Rand(seed) < weights.pSpecular)
    {
        float roughness = surface.roughness;
        float3 H = SampleGGXVNDFWorld(surface.V, surface.N, roughness, float2(Rand(seed), Rand(seed)));
        sample.wi = reflect(-surface.V, H);

        float NoL = saturate(dot(surface.N, sample.wi));
        float VoH = saturate(dot(surface.V, H));
        if (NoL <= 0.0f || VoH <= 0.0f)
        {
            return false;
        }

        float specularPdf = SpecularReflectionPdf(surface, sample.wi);
        sample.f = EvaluateSpecularBrdf(surface, sample.wi);
        sample.pdf = max(weights.pSpecular * specularPdf, 1e-6f);
        sample.wasSpecular = true;
        return true;
    }

    if (weights.pDiffuse <= 0.0f)
    {
        return false;
    }

    sample.wi = SampleCosineHemisphereWorld(float2(Rand(seed), Rand(seed)), surface.N);
    float NoL = saturate(dot(surface.N, sample.wi));
    if (NoL <= 0.0f)
    {
        return false;
    }

    sample.f = EvaluateDiffuseBrdf(surface, sample.wi);
    sample.pdf = max(weights.pDiffuse * CosineHemispherePdf(NoL), 1e-6f);
    sample.wasSpecular = false;
    return true;
}

float ComputeSpecularHitDistanceGuide(RaytracingAccelerationStructure scene, SurfaceState surface, bool use2StateOmmRays, bool allowReorder)
{
    float3 specularDir = reflect(-surface.V, surface.N);
    float NoL = saturate(dot(surface.N, specularDir));
    float GoL = saturate(dot(surface.geoN, specularDir));
    if (NoL <= 0.0f || GoL <= 0.0f)
    {
        return 0.0f;
    }

    RayPayload payload;
    TracePayloadWithBiasAndTMin(scene, use2StateOmmRays, allowReorder, surface.pos, surface.geoN, specularDir, RT_PRIMARY_BOUNCE_CONTINUATION_RAY_ORIGIN_BIAS, RT_RAY_EPS, payload);
    if (payload.hit == 0u)
    {
        return 0.0f;
    }

    return length(payload.hitPos - surface.pos);
}

PathResult IntegratePath(RaytracingAccelerationStructure scene, SurfaceState firstSurface)
{
    PathResult result;
    result.radiance = 0.0f.xxx;
    result.specHitDistance = 0.0f;

    uint spp = Constants.rtPathTraceSpp;
    const bool use2StateOmmRays = (Constants.rtOmmsEnabled != 0u) && (Constants.rtUse2StateOmmRays != 0u);
    const bool useSer = Constants.rtSerEnabled != 0u;
    SunLightData sun = BuildSunLightData();
    uint2 px = DispatchRaysIndex().xy;
    uint baseSeed = HashUint(px.x * 1973u + px.y * 9277u + 89173u + Constants.frameIndex * 26699u);
    result.specHitDistance = ComputeSpecularHitDistanceGuide(scene, firstSurface, use2StateOmmRays, useSer);

    for (uint s = 0u; s < spp; ++s)
    {
        uint seed = baseSeed ^ (s * 0x9E3779B9u);
        SurfaceState surface = firstSurface;
        float3 throughput = 1.0f.xxx;
        float3 radiance = surface.emissive;

        for (uint bounce = 0u; bounce < Constants.rtMaxBounces; ++bounce)
        {
            BsdfMixtureWeights weights = ComputeBsdfMixtureWeights(surface);

            float3 direct = EvaluateSunDirect(scene, surface, weights, sun, seed);
            radiance += throughput * direct;

            BsdfSample sample;
            if (!SamplePathEvent(surface, weights, seed, sample))
            {
                break;
            }

            float NoL = saturate(dot(surface.N, sample.wi));
            throughput *= sample.f * (NoL / sample.pdf);
            if (MaxComponent(throughput) <= 1e-4f)
            {
                break;
            }

            RayPayload payload;
            float originBias = (bounce == 0u) ? RT_PRIMARY_BOUNCE_CONTINUATION_RAY_ORIGIN_BIAS : RT_RAY_EPS;
            TracePayloadWithBiasAndTMin(scene, use2StateOmmRays, useSer, surface.pos, surface.geoN, sample.wi, originBias, RT_RAY_EPS, payload);

            if (payload.hit == 0u)
            {
                bool hitSun = IsDirectionInsideSunCone(sample.wi, sun);
                float lightPdf = hitSun ? sun.conePdf : 0.0f;
                float bsdfPdf = ComputePathEventPdf(surface, sample.wi, weights);
                float misWeight = (lightPdf > 0.0f) ? PowerHeuristic(bsdfPdf, lightPdf) : 1.0f;
                float3 missRadiance = hitSun ? (sun.coneRadiance * misWeight) : SampleSkyBackground(sample.wi);

                radiance += throughput * missRadiance;
                break;
            }

            radiance += throughput * payload.emissive;
            surface = SurfaceFromPayload(payload, -sample.wi);

            if (bounce >= PATH_TRACE_RUSSIAN_ROULETTE_MIN_BOUNCE)
            {
                // Russian roulette terminates low-energy paths while keeping the estimator unbiased.
                float survivalProbability = ComputeRussianRouletteSurvivalProbability(throughput);
                if (Rand(seed) > survivalProbability)
                {
                    break;
                }
                throughput /= survivalProbability;
            }
        }

        result.radiance += radiance;
    }

    float invSpp = rcp((float)spp);
    result.radiance *= invSpp;
    return result;
}

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.outputTextureIndex];
    RWTexture2D<float> hitDistanceOutput = ResourceDescriptorHeap[Constants.hitDistanceTextureIndex];

    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    Texture2D<float2> normalGeoTex = ResourceDescriptorHeap[Constants.normalGeoTextureIndex];

    uint2 px = DispatchRaysIndex().xy;
    float2 uv = (float2(px) + 0.5f) * Constants.fullDimInv;
    float depth = depthTex.Load(int3(px, 0)).r;

    if (IsSkyDepth(depth))
    {
        float3 V = normalize(Constants.cameraPos - ReconstructWorldPos(uv, depth + 1e-6f, Constants.invViewProj));
        float3 skyColor = SampleSkyBackground(-V);
        output[px] = float4(skyColor, 1.0f);
        hitDistanceOutput[px] = 0.0f;
        return;
    }

    float3 worldPos = ReconstructWorldPos(uv, depth, Constants.invViewProj);
    float3 geoNormal = DecodeNormal(normalGeoTex.Load(int3(px, 0)));
    geoNormal = normalize(geoNormal);

    SurfaceState surface = GatherSurface(px, worldPos, geoNormal);
    if (Constants.debugMeshletColorEnabled != 0u)
    {
        output[px] = float4(surface.albedo, 1.0f);
        hitDistanceOutput[px] = 0.0f;
        return;
    }
    if (Constants.debugViewMode == PathTraceDebugView_SurfaceNormals)
    {
        output[px] = float4(surface.N * 0.5f + 0.5f, 1.0f);
        hitDistanceOutput[px] = 0.0f;
        return;
    }

    PathResult path = IntegratePath(scene, surface);
    output[px] = float4(min(max(path.radiance, 0.0.xxx), 65504.0.xxx), 1.0f);
    hitDistanceOutput[px] = path.specHitDistance;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    ClosestHitDefault(payload, attr, WorldRayDirection(), Constants.primInfoBufferIndex, Constants.materialsBufferIndex);
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    AnyHitDefault(payload, attr, Constants.primInfoBufferIndex, Constants.materialsBufferIndex);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    MissDefault(payload);
}
