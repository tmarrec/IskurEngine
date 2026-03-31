// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "include/core/math_constants.hlsli"
#include "CPUGPU.h"
#include "include/geometry/normal.hlsli"

static const float RT_RAY_EPS = 0.001f;

struct [raypayload] RayPayload
{
    uint hit : read(caller, closesthit) : write(caller, closesthit);
    float3 hitPos : read(caller, closesthit) : write(caller, closesthit);
    float3 hitN : read(caller, closesthit) : write(caller, closesthit);
    float3 albedo : read(caller, closesthit) : write(caller, closesthit);
    float3 emissive : read(caller, closesthit) : write(caller, closesthit);
};

bool RTIsSkyDepth(float depth)
{
    return depth <= 1e-6f; // reversed-Z clear=0
}

uint RTHashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float RTRand(inout uint seed)
{
    seed = RTHashUint(seed);
    return (seed & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

void RTBuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;

    t = float3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    b = float3(bb, sign + n.y * n.y * a, -n.y);
}

float3 RTSampleCosineHemisphere(float2 u)
{
    float r = sqrt(u.x);
    float a = IE_TWO_PI * u.y;
    return float3(r * cos(a), r * sin(a), sqrt(max(0.0f, 1.0f - u.x)));
}

float3 RTSampleCosineHemisphereWorld(float2 u, float3 n)
{
    float3 t, b;
    RTBuildOrthonormalBasis(n, t, b);
    float3 l = RTSampleCosineHemisphere(u);
    return t * l.x + b * l.y + n * l.z;
}

float3 RTSampleAroundDirection(float3 dir, float spread, float2 u)
{
    float3 t, b;
    RTBuildOrthonormalBasis(dir, t, b);
    float3 l = RTSampleCosineHemisphere(u);
    float3 d = normalize(t * l.x + b * l.y + dir * l.z);
    return normalize(lerp(dir, d, saturate(spread)));
}

void RTInitPayload(out RayPayload payload)
{
    payload.hit = 0u;
    payload.hitPos = 0.0f.xxx;
    payload.hitN = 0.0f.xxx;
    payload.albedo = 0.0f.xxx;
    payload.emissive = 0.0f.xxx;
}

void RTTracePayloadWithBiasAndTMin(RaytracingAccelerationStructure scene, float3 origin, float3 surfaceN, float3 rayDir, float originBias, float rayTMin, out RayPayload payload)
{
    RTInitPayload(payload);

    RayDesc ray;
    ray.Origin = origin + surfaceN * originBias;
    ray.Direction = rayDir;
    ray.TMin = rayTMin;
    ray.TMax = 1e6f;

    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
}

void RTTracePayload(RaytracingAccelerationStructure scene, float3 origin, float3 surfaceN, float3 rayDir, out RayPayload payload)
{
    RTTracePayloadWithBiasAndTMin(scene, origin, surfaceN, rayDir, RT_RAY_EPS, RT_RAY_EPS, payload);
}

float3 RTNormalizeSunTint(float3 sunColor)
{
    float3 tint = max(sunColor, 0.0.xxx);
    float luminance = dot(tint, float3(0.2126f, 0.7152f, 0.0722f));
    return (luminance > 1e-4f) ? (tint / luminance) : 0.0.xxx;
}

float3 RTEvaluateOneBounceSkySun(
    RaytracingAccelerationStructure scene,
    float3 origin,
    float3 surfaceN,
    float3 rayDir,
    float3 sunDir,
    float3 sunColor,
    float sunIntensity,
    u32 skyCubeIndex,
    u32 samplerIndex,
    float skyIntensity)
{
    RayPayload payload;
    RTTracePayload(scene, origin, surfaceN, rayDir, payload);

    SamplerState envSamp = SamplerDescriptorHeap[samplerIndex];
    TextureCube<float4> skyCube = ResourceDescriptorHeap[skyCubeIndex];
    float3 skyColor = skyCube.SampleLevel(envSamp, rayDir, 0.0f).rgb * skyIntensity;

    if (payload.hit == 0u)
    {
        return skyColor;
    }

    float3 sunL = normalize(-sunDir);
    float NdotL = saturate(dot(payload.hitN, sunL));
    float3 hitDiffuse = payload.albedo * (RTNormalizeSunTint(sunColor) * sunIntensity) * (NdotL * IE_INV_PI);
    float3 hitSkyFill = payload.albedo * skyCube.SampleLevel(envSamp, payload.hitN, 0.0f).rgb * (skyIntensity * 0.15f);
    return payload.emissive + hitDiffuse + hitSkyFill;
}

void RTFetchTriangleMaterialDataExplicit(uint instanceId, uint primitiveIndex, float2 barycentrics, u32 primInfoBufferIndex, u32 materialsBufferIndex, out Material material, out float2 uv,
                                         out float4 vertexColor)
{
    StructuredBuffer<RTPrimInfo> primInfos = ResourceDescriptorHeap[primInfoBufferIndex];
    RTPrimInfo info = primInfos[instanceId];

    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[info.vbSrvIndex];
    StructuredBuffer<uint> ib = ResourceDescriptorHeap[info.ibSrvIndex];

    uint tri = primitiveIndex;
    uint i0 = ib[tri * 3 + 0];
    uint i1 = ib[tri * 3 + 1];
    uint i2 = ib[tri * 3 + 2];

    Vertex v0 = vb[i0];
    Vertex v1 = vb[i1];
    Vertex v2 = vb[i2];

    float b1 = barycentrics.x;
    float b2 = barycentrics.y;
    float b0 = 1.0f - b1 - b2;

    float2 uv0 = DecodePackedHalf2(v0.texCoordPacked);
    float2 uv1 = DecodePackedHalf2(v1.texCoordPacked);
    float2 uv2 = DecodePackedHalf2(v2.texCoordPacked);
    uv = uv0 * b0 + uv1 * b1 + uv2 * b2;

    float4 c0 = DecodePackedColorRGBA16UNORM(v0.colorPackedLo, v0.colorPackedHi);
    float4 c1 = DecodePackedColorRGBA16UNORM(v1.colorPackedLo, v1.colorPackedHi);
    float4 c2 = DecodePackedColorRGBA16UNORM(v2.colorPackedLo, v2.colorPackedHi);
    vertexColor = c0 * b0 + c1 * b1 + c2 * b2;

    StructuredBuffer<Material> mats = ResourceDescriptorHeap[materialsBufferIndex];
    material = mats[info.materialIdx];
}

void RTFetchTriangleMaterialData(in BuiltInTriangleIntersectionAttributes attr, u32 primInfoBufferIndex, u32 materialsBufferIndex, out Material material, out float2 uv, out float4 vertexColor)
{
    RTFetchTriangleMaterialDataExplicit(InstanceID(), PrimitiveIndex(), attr.barycentrics, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);
}

float4 RTLoadBaseColor(in Material material, float2 uv, float4 vertexColor)
{
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState samp = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= tex.SampleLevel(samp, uv, 0.0f);
    }
    baseColor *= vertexColor;
    if (any(baseColor != baseColor) || any(abs(baseColor) > 1e20.xxxx))
    {
        return 0.0.xxxx;
    }
    return baseColor;
}

bool RTPassesAlphaTest(in BuiltInTriangleIntersectionAttributes attr, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    Material material;
    float2 uv;
    float4 vertexColor;
    RTFetchTriangleMaterialData(attr, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);

    static const uint RT_ALPHA_MODE_MASK = 2u;
    if (material.alphaMode != RT_ALPHA_MODE_MASK)
    {
        return true;
    }

    float4 baseColor = RTLoadBaseColor(material, uv, vertexColor);
    return baseColor.a >= material.alphaCutoff;
}

bool RTPassesAlphaTestExplicit(uint instanceId, uint primitiveIndex, float2 barycentrics, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    Material material;
    float2 uv;
    float4 vertexColor;
    RTFetchTriangleMaterialDataExplicit(instanceId, primitiveIndex, barycentrics, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);

    static const uint RT_ALPHA_MODE_MASK = 2u;
    if (material.alphaMode != RT_ALPHA_MODE_MASK)
    {
        return true;
    }

    float4 baseColor = RTLoadBaseColor(material, uv, vertexColor);
    return baseColor.a >= material.alphaCutoff;
}

void RTFillPayloadFromTriangleHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr, float3 rayDir, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    StructuredBuffer<RTPrimInfo> primInfos = ResourceDescriptorHeap[primInfoBufferIndex];
    RTPrimInfo info = primInfos[InstanceID()];

    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[info.vbSrvIndex];
    StructuredBuffer<uint> ib = ResourceDescriptorHeap[info.ibSrvIndex];

    uint tri = PrimitiveIndex();
    uint i0 = ib[tri * 3 + 0];
    uint i1 = ib[tri * 3 + 1];
    uint i2 = ib[tri * 3 + 2];

    Vertex v0 = vb[i0];
    Vertex v1 = vb[i1];
    Vertex v2 = vb[i2];

    float b1 = attr.barycentrics.x;
    float b2 = attr.barycentrics.y;
    float b0 = 1.0f - b1 - b2;

    float3 n0Obj = DecodePackedNormalOct(v0.normalPacked);
    float3 n1Obj = DecodePackedNormalOct(v1.normalPacked);
    float3 n2Obj = DecodePackedNormalOct(v2.normalPacked);
    float3 nObj = normalize(n0Obj * b0 + n1Obj * b1 + n2Obj * b2);

    float3x3 w2o = (float3x3)WorldToObject3x4();
    float3 nWorld = normalize(mul(transpose(w2o), nObj));
    if (dot(nWorld, -rayDir) < 0.0f)
    {
        nWorld = -nWorld;
    }
    payload.hitN = nWorld;

    Material m;
    float2 uv;
    float4 vertexColor;
    RTFetchTriangleMaterialData(attr, primInfoBufferIndex, materialsBufferIndex, m, uv, vertexColor);

    float4 baseColor = RTLoadBaseColor(m, uv, vertexColor);
    float3 albedo = baseColor.rgb;
    payload.albedo = saturate(albedo);

    float3 emissive = max(m.emissiveFactor, 0.0.xxx);
    if (m.emissiveTextureIndex != -1)
    {
        Texture2D<float4> emissiveTex = ResourceDescriptorHeap[m.emissiveTextureIndex];
        SamplerState emissiveSamp = SamplerDescriptorHeap[m.emissiveSamplerIndex];
        emissive *= emissiveTex.SampleLevel(emissiveSamp, uv, 0.0f).rgb;
    }
    if (any(emissive != emissive) || any(abs(emissive) > 1e20.xxx))
    {
        emissive = 0.0.xxx;
    }
    payload.emissive = max(emissive, 0.0.xxx);
}
