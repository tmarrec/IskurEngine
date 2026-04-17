// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/core/math_constants.hlsli"

static const uint ATM_VIEW_STEPS_HORIZON = 64;
static const uint ATM_SUN_STEPS_HORIZON = 16;

float3 CubeFaceUvToDir(uint face, float2 uv)
{
    // uv is expected in [-1, 1] range.
    switch (face)
    {
    case 0:
        return normalize(float3(1.0, -uv.y, -uv.x)); // +X
    case 1:
        return normalize(float3(-1.0, -uv.y, uv.x)); // -X
    case 2:
        return normalize(float3(uv.x, 1.0, uv.y)); // +Y
    case 3:
        return normalize(float3(uv.x, -1.0, -uv.y)); // -Y
    case 4:
        return normalize(float3(uv.x, -uv.y, 1.0)); // +Z
    default:
        return normalize(float3(-uv.x, -uv.y, -1.0)); // -Z
    }
}

bool AnyInvalid3(float3 v)
{
    // NaN != NaN; the magnitude test also catches inf-like blowups.
    return any(v != v) || any(abs(v) > 1e20.xxx);
}

float2 RaySphereIntersect(float3 rayOrigin, float3 rayDir, float radius)
{
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - radius * radius;
    float h = b * b - c;
    if (h < 0.0)
    {
        return float2(1.0, -1.0);
    }
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

float3 IntegrateAtmosphereSingleScattering(float3 viewDir, float3 sunDir, float sunIntensity, float3 rayleighScattering, float rayleighScaleHeightKm, float3 mieScattering,
                                           float mieScaleHeightKm, float mieG, float atmosphereThicknessKm, float atmosphereSunIntensityScale, float3 ozoneAbsorption,
                                           float ozoneLayerCenterKm, float ozoneLayerWidthKm, float multiScatteringStrength)
{
    // Clamp UI-driven parameters to sane atmospheric ranges.
    float3 betaR = max(rayleighScattering, 0.0.xxx);
    float3 betaM = max(mieScattering, 0.0.xxx);
    float Hr = max(rayleighScaleHeightKm, 0.05);
    float Hm = max(mieScaleHeightKm, 0.02);
    float3 betaO = max(ozoneAbsorption, 0.0.xxx);
    float ozoneCenter = max(ozoneLayerCenterKm, 0.0);
    float ozoneWidth = max(ozoneLayerWidthKm, 0.1);
    float g = clamp(mieG, -0.98, 0.98);
    float thicknessKm = max(atmosphereThicknessKm, 1.0);
    float sunScale = max(atmosphereSunIntensityScale, 0.0);
    float msStrength = max(multiScatteringStrength, 0.0);

    const float planetRadiusKm = 6360.0;
    const float cameraAltitudeKm = 0.001;
    const float atmosphereRadiusKm = planetRadiusKm + thicknessKm;

    float3 rayOrigin = float3(0.0, planetRadiusKm + cameraAltitudeKm, 0.0);
    float3 rayDir = normalize(viewDir);
    float3 sunViewDir = normalize(-sunDir);

    // Fixed step counts avoid banding from neighboring rays taking different paths.
    uint viewSteps = ATM_VIEW_STEPS_HORIZON;
    uint sunSteps = ATM_SUN_STEPS_HORIZON;

    float2 atmosphereHit = RaySphereIntersect(rayOrigin, rayDir, atmosphereRadiusKm);
    if (atmosphereHit.x > atmosphereHit.y)
    {
        return 0.0.xxx;
    }

    float tMin = max(atmosphereHit.x, 0.0);
    float tMax = max(atmosphereHit.y, 0.0);
    if (tMax <= tMin)
    {
        return 0.0.xxx;
    }

    float2 groundHit = RaySphereIntersect(rayOrigin, rayDir, planetRadiusKm);
    if (groundHit.x > 0.0)
    {
        tMax = min(tMax, groundHit.x);
    }
    if (tMax <= tMin)
    {
        return 0.0.xxx;
    }

    float mu = dot(rayDir, sunViewDir);
    float mu2 = mu * mu;
    float phaseR = (3.0 / (16.0 * IE_PI)) * (1.0 + mu2);

    float g2 = g * g;
    float denomM = pow(max(1e-4, 1.0 + g2 - 2.0 * g * mu), 1.5);
    float phaseM = (1.0 - g2) / (4.0 * IE_PI * denomM);

    float segmentLength = (tMax - tMin) / (float)viewSteps;
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;
    float opticalDepthO = 0.0;
    float3 radiance = 0.0.xxx;
    float3 multiScatterAccum = 0.0.xxx;

    [loop]
    for (uint i = 0; i < viewSteps; ++i)
    {
        float t = tMin + ((float)i + 0.5) * segmentLength;
        float3 samplePos = rayOrigin + rayDir * t;
        float sampleRadiusKm = length(samplePos);
        float heightKm = max(0.0, sampleRadiusKm - planetRadiusKm);
        float densityR = exp(-heightKm / Hr);
        float densityM = exp(-heightKm / Hm);
        float ozoneX = (heightKm - ozoneCenter) / ozoneWidth;
        float densityO = exp(-0.5 * ozoneX * ozoneX);

        opticalDepthR += densityR * segmentLength;
        opticalDepthM += densityM * segmentLength;
        opticalDepthO += densityO * segmentLength;
        // Smooth the horizon test to avoid contouring.
        float towardPlanet = step(dot(samplePos, sunViewDir), 0.0);
        float lineClearanceKm = length(cross(samplePos, sunViewDir)) - planetRadiusKm;
        float sunBlocked = towardPlanet * (1.0 - smoothstep(-1.5, 1.5, lineClearanceKm));
        float sunVisibility = saturate(1.0 - sunBlocked);
        if (sunVisibility <= 1e-4)
        {
            continue;
        }

        float2 sunAtmHit = RaySphereIntersect(samplePos, sunViewDir, atmosphereRadiusKm);
        float tSunMax = max(sunAtmHit.y, 0.0);
        if (tSunMax <= 0.0)
        {
            continue;
        }

        float sunSegmentLength = tSunMax / (float)sunSteps;
        float opticalDepthRToSun = 0.0;
        float opticalDepthMToSun = 0.0;
        float opticalDepthOToSun = 0.0;

        [loop]
        for (uint j = 0; j < sunSteps; ++j)
        {
            float tSun = ((float)j + 0.5) * sunSegmentLength;
            float3 sunSamplePos = samplePos + sunViewDir * tSun;
            float sunHeightKm = max(0.0, length(sunSamplePos) - planetRadiusKm);
            opticalDepthRToSun += exp(-sunHeightKm / Hr) * sunSegmentLength;
            opticalDepthMToSun += exp(-sunHeightKm / Hm) * sunSegmentLength;
            float ozoneXSun = (sunHeightKm - ozoneCenter) / ozoneWidth;
            opticalDepthOToSun += exp(-0.5 * ozoneXSun * ozoneXSun) * sunSegmentLength;
        }

        float3 tau = betaR * (opticalDepthR + opticalDepthRToSun) + betaM * (opticalDepthM + opticalDepthMToSun) + betaO * (opticalDepthO + opticalDepthOToSun);
        float3 transmittance = exp(-tau);
        float3 scattering = densityR * betaR * phaseR + densityM * betaM * phaseM;
        float3 single = transmittance * scattering * (segmentLength * sunVisibility);
        radiance += single;
        multiScatterAccum += single * (1.0.xxx - transmittance);
    }

    float sunStrength = max(0.0, sunIntensity) * sunScale;
    float3 result = max((radiance + multiScatterAccum * msStrength) * sunStrength, 0.0.xxx);
    if (AnyInvalid3(result))
    {
        return 0.0.xxx;
    }
    return result;
}

float3 ApplySunDiskAndGlow(float3 base, float3 viewDir, uint skyCubeSize, float3 sunDir, float sunIntensity, float3 sunColor, float sunDiskAngleDeg, float sunDiskSoftness,
                           float sunGlowPower, float sunGlowIntensity, float sunDiskIntensityScale)
{
    float clampedSunDiskAngleDeg = clamp(sunDiskAngleDeg, 0.001, 12.0);
    float sunDiskCosAngle = cos(radians(clampedSunDiskAngleDeg));
    float3 sunViewDir = normalize(-sunDir);
    float sunDot = saturate(dot(viewDir, sunViewDir));

    // Derive a minimum disk width from the cubemap texel footprint so tiny suns still antialias.
    float baseWidth = max(1e-5, (1.0 - sunDiskCosAngle) * 0.5);
    float minTexelAngleDeg = 22.5 / max((float)skyCubeSize, 1.0);
    float minTexelCosAngle = cos(radians(minTexelAngleDeg));
    float minTexelWidth = max(1e-8, (1.0 - minTexelCosAngle) * 0.5);
    float diskWidth = max(baseWidth * max(sunDiskSoftness, 0.01), minTexelWidth);
    float sunDisk = smoothstep(sunDiskCosAngle - diskWidth, sunDiskCosAngle + diskWidth, sunDot);
    float sunGlow = pow(sunDot, max(sunGlowPower, 1.0));

    float sunStrength = max(0.0, sunIntensity) * max(0.0, sunDiskIntensityScale);
    base += sunColor * (sunStrength * sunDisk + max(0.0, sunGlowIntensity) * sunStrength * sunGlow);
    return base;
}

float3 EvaluateProceduralSkyBase(float3 viewDir, float3 sunDir, float sunIntensity, float atmosphereThicknessKm, float atmosphereSunIntensityScale, float mieG,
                                 float3 rayleighScattering, float rayleighScaleHeightKm, float3 mieScattering, float mieScaleHeightKm, float3 ozoneAbsorption,
                                 float ozoneLayerCenterKm, float ozoneLayerWidthKm, float multiScatteringStrength)
{
    float3 originalViewDir = normalize(viewDir);
    float3 mirroredViewDir = originalViewDir;
    bool isUpperHemisphere = originalViewDir.y >= 0.0f;
    // Mirror the lower hemisphere, but keep solar highlights in the upper one.
    mirroredViewDir.y = abs(mirroredViewDir.y);

    // Suppress Mie sun glow below the horizon when mirroring the sky.
    float3 mieForAtmo = isUpperHemisphere ? mieScattering : 0.0.xxx;
    float mieGForAtmo = isUpperHemisphere ? mieG : 0.0f;
    return IntegrateAtmosphereSingleScattering(mirroredViewDir, sunDir, sunIntensity, rayleighScattering, rayleighScaleHeightKm, mieForAtmo, mieScaleHeightKm, mieGForAtmo,
                                               atmosphereThicknessKm, atmosphereSunIntensityScale, ozoneAbsorption, ozoneLayerCenterKm, ozoneLayerWidthKm,
                                               multiScatteringStrength);
}

float3 AddProceduralSunVisuals(float3 base, float3 viewDir, uint skyCubeSize, float3 sunDir, float sunIntensity, float3 sunColor, float sunDiskAngleDeg, float sunDiskSoftness,
                               float sunGlowPower, float sunGlowIntensity, float sunDiskIntensityScale)
{
    float3 originalViewDir = normalize(viewDir);
    bool isUpperHemisphere = originalViewDir.y >= 0.0f;
    sunColor = max(sunColor, 0.0.xxx);

    // Only render the sun in the upper hemisphere.
    if (isUpperHemisphere)
    {
        base = ApplySunDiskAndGlow(base, originalViewDir, skyCubeSize, sunDir, sunIntensity, sunColor, sunDiskAngleDeg, sunDiskSoftness, sunGlowPower, sunGlowIntensity,
                                   sunDiskIntensityScale);
    }
    return max(base, 0.0.xxx);
}

ConstantBuffer<SkyCubeGenConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=36, b0)"

[RootSignature(ROOT_SIG)]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Constants.size || tid.y >= Constants.size || tid.z >= 6)
    {
        return;
    }

    RWTexture2DArray<float4> skyCube = ResourceDescriptorHeap[Constants.outUavIndex];

    float2 uv = ((float2)tid.xy + 0.5) / (float)Constants.size;
    uv = uv * 2.0 - 1.0;

    float3 dir = CubeFaceUvToDir(tid.z, uv);
    float3 skyBase = EvaluateProceduralSkyBase(dir, Constants.sunDir, Constants.sunIntensity, Constants.atmosphereThicknessKm, Constants.atmosphereSunIntensityScale, Constants.mieG,
                                               Constants.rayleighScattering, Constants.rayleighScaleHeightKm, Constants.mieScattering, Constants.mieScaleHeightKm,
                                               Constants.ozoneAbsorption, Constants.ozoneLayerCenterKm, Constants.ozoneLayerWidthKm, Constants.multiScatteringStrength);
    float3 skyWithSun = AddProceduralSunVisuals(skyBase, dir, Constants.size, Constants.sunDir, Constants.sunIntensity, Constants.sunColor, Constants.sunDiskAngleDeg,
                                                Constants.sunDiskSoftness, Constants.sunGlowPower, Constants.sunGlowIntensity, Constants.sunDiskIntensityScale);

    skyCube[uint3(tid.xy, tid.z)] = float4(max(skyWithSun, 0.0.xxx), 1.0);
}

