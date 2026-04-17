// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "include/core/hash.hlsli"
#include "include/core/math_constants.hlsli"
#include "include/core/sun.hlsli"
#include "CPUGPU.h"
#include "include/geometry/normal.hlsli"

static const float RT_RAY_EPS = 0.001f;
static const uint RT_RAY_MASK = 0xFFu;

struct [raypayload] RayPayload
{
    uint hit : read(caller, closesthit) : write(caller, closesthit);
    float3 hitPos : read(caller, closesthit) : write(caller, closesthit);
    float3 geoN : read(caller, closesthit) : write(caller, closesthit);
    float3 hitN : read(caller, closesthit) : write(caller, closesthit);
    float3 albedo : read(caller, closesthit) : write(caller, closesthit);
    float3 emissive : read(caller, closesthit) : write(caller, closesthit);
    float roughness : read(caller, closesthit) : write(caller, closesthit);
    float metalness : read(caller, closesthit) : write(caller, closesthit);
};

bool PassesAlphaTestExplicit(uint instanceId, uint primitiveIndex, float2 barycentrics, u32 primInfoBufferIndex, u32 materialsBufferIndex);

bool IsSkyDepth(float depth)
{
    return depth <= 1e-6f; // reversed-Z clear=0
}

float Rand(inout uint seed)
{
    seed = HashUint(seed);
    return (seed & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;

    t = float3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    b = float3(bb, sign + n.y * n.y * a, -n.y);
}

float3 SampleCosineHemisphere(float2 u)
{
    float r = sqrt(u.x);
    float a = IE_TWO_PI * u.y;
    return float3(r * cos(a), r * sin(a), sqrt(max(0.0f, 1.0f - u.x)));
}

float3 SampleCosineHemisphereWorld(float2 u, float3 n)
{
    float3 t, b;
    BuildOrthonormalBasis(n, t, b);
    float3 l = SampleCosineHemisphere(u);
    return t * l.x + b * l.y + n * l.z;
}

void InitPayload(out RayPayload payload)
{
    payload.hit = 0u;
    payload.hitPos = 0.0f.xxx;
    payload.geoN = 0.0f.xxx;
    payload.hitN = 0.0f.xxx;
    payload.albedo = 0.0f.xxx;
    payload.emissive = 0.0f.xxx;
    payload.roughness = 0.0f;
    payload.metalness = 0.0f;
}

uint GetOmmRayFlags(bool use2StateOmmRays)
{
    return use2StateOmmRays ? RAY_FLAG_FORCE_OMM_2_STATE : RAY_FLAG_NONE;
}

RayDesc BuildRayDesc(float3 origin, float3 surfaceN, float3 rayDir, float originBias, float rayTMin)
{
    RayDesc ray;
    ray.Origin = origin + surfaceN * originBias;
    ray.Direction = rayDir;
    ray.TMin = rayTMin;
    ray.TMax = 1e6f;
    return ray;
}

void TracePayloadWithFlagsAndBiasAndTMin(RaytracingAccelerationStructure scene, uint rayFlags, bool allowReorder, float3 origin, float3 surfaceN, float3 rayDir, float originBias,
                                         float rayTMin, out RayPayload payload)
{
    InitPayload(payload);
    RayDesc ray = BuildRayDesc(origin, surfaceN, rayDir, originBias, rayTMin);
    dx::HitObject hit = dx::HitObject::TraceRay(scene, rayFlags, RT_RAY_MASK, 0, 1, 0, ray, payload);
    if (allowReorder)
    {
        dx::MaybeReorderThread(hit);
    }
    dx::HitObject::Invoke(hit, payload);
}

void TracePayloadWithBiasAndTMin(RaytracingAccelerationStructure scene, bool use2StateOmmRays, bool allowReorder, float3 origin, float3 surfaceN, float3 rayDir, float originBias,
                                 float rayTMin, out RayPayload payload)
{
    TracePayloadWithFlagsAndBiasAndTMin(scene, GetOmmRayFlags(use2StateOmmRays), allowReorder, origin, surfaceN, rayDir, originBias, rayTMin, payload);
}

float TraceVisibilityWithBias(RaytracingAccelerationStructure scene, float3 hitPos, float3 hitN, float3 rayDir, float originBias, float rayTMin, bool use2StateOmmRays,
                              u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    RayDesc ray = BuildRayDesc(hitPos, hitN, rayDir, originBias, rayTMin);
    const uint flags = GetOmmRayFlags(use2StateOmmRays) | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> q;
    q.TraceRayInline(scene, flags, RT_RAY_MASK, ray);
    while (q.Proceed())
    {
        if (!use2StateOmmRays && q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (PassesAlphaTestExplicit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), primInfoBufferIndex, materialsBufferIndex))
            {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

void FetchTriangleMaterialDataExplicit(uint instanceId, uint primitiveIndex, float2 barycentrics, u32 primInfoBufferIndex, u32 materialsBufferIndex, out Material material, out float2 uv,
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

void FetchTriangleMaterialData(in BuiltInTriangleIntersectionAttributes attr, u32 primInfoBufferIndex, u32 materialsBufferIndex, out Material material, out float2 uv, out float4 vertexColor)
{
    FetchTriangleMaterialDataExplicit(InstanceID(), PrimitiveIndex(), attr.barycentrics, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);
}

float4 LoadBaseColor(in Material material, float2 uv, float4 vertexColor)
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

bool PassesAlphaTest(in BuiltInTriangleIntersectionAttributes attr, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    Material material;
    float2 uv;
    float4 vertexColor;
    FetchTriangleMaterialData(attr, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);

    static const uint RT_ALPHA_MODE_MASK = 2u;
    if (material.alphaMode != RT_ALPHA_MODE_MASK)
    {
        return true;
    }

    float4 baseColor = LoadBaseColor(material, uv, vertexColor);
    return baseColor.a >= material.alphaCutoff;
}

bool PassesAlphaTestExplicit(uint instanceId, uint primitiveIndex, float2 barycentrics, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    Material material;
    float2 uv;
    float4 vertexColor;
    FetchTriangleMaterialDataExplicit(instanceId, primitiveIndex, barycentrics, primInfoBufferIndex, materialsBufferIndex, material, uv, vertexColor);

    static const uint RT_ALPHA_MODE_MASK = 2u;
    if (material.alphaMode != RT_ALPHA_MODE_MASK)
    {
        return true;
    }

    float4 baseColor = LoadBaseColor(material, uv, vertexColor);
    return baseColor.a >= material.alphaCutoff;
}

void FillPayloadFromTriangleHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr, float3 rayDir, u32 primInfoBufferIndex, u32 materialsBufferIndex)
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

    float3 faceNObj = cross(v1.position - v0.position, v2.position - v0.position);
    float faceNLenSq = dot(faceNObj, faceNObj);
    float3 n0Obj = DecodePackedNormalOct(v0.normalPacked);
    float3 n1Obj = DecodePackedNormalOct(v1.normalPacked);
    float3 n2Obj = DecodePackedNormalOct(v2.normalPacked);
    float3 shadeNObj = normalize(n0Obj * b0 + n1Obj * b1 + n2Obj * b2);
    float3x3 w2o = (float3x3)WorldToObject3x4();
    float3 geoNWorld = (faceNLenSq > 1e-8f) ? normalize(mul(transpose(w2o), faceNObj)) : normalize(mul(transpose(w2o), shadeNObj));
    float3 shadeNWorld = normalize(mul(transpose(w2o), shadeNObj));
    float normalFlip = (dot(geoNWorld, -rayDir) < 0.0f) ? -1.0f : 1.0f;
    geoNWorld *= normalFlip;
    shadeNWorld *= normalFlip;
    payload.geoN = geoNWorld;

    Material m;
    float2 uv;
    float4 vertexColor;
    FetchTriangleMaterialData(attr, primInfoBufferIndex, materialsBufferIndex, m, uv, vertexColor);

    float4 baseColor = LoadBaseColor(m, uv, vertexColor);
    float3 albedo = baseColor.rgb;
    payload.albedo = saturate(albedo);

    float metalness = saturate(m.metallicFactor);
    float roughness = saturate(m.roughnessFactor);
    if (m.metallicRoughnessTextureIndex != -1)
    {
        Texture2D<float4> metallicRoughnessTex = ResourceDescriptorHeap[m.metallicRoughnessTextureIndex];
        SamplerState metallicRoughnessSamp = SamplerDescriptorHeap[m.metallicRoughnessSamplerIndex];
        float4 metallicRoughness = metallicRoughnessTex.SampleLevel(metallicRoughnessSamp, uv, 0.0f);
        metalness *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    payload.metalness = saturate(metalness);
    payload.roughness = saturate(roughness);

    float3 shadingN = shadeNWorld;
    if (dot(shadingN, geoNWorld) <= 0.0f)
    {
        shadingN = geoNWorld;
    }
    payload.hitN = shadingN;

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

void ClosestHitDefault(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr, float3 rayDir, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    payload.hit = 1u;
    payload.hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    FillPayloadFromTriangleHit(payload, attr, rayDir, primInfoBufferIndex, materialsBufferIndex);
}

void AnyHitDefault(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr, u32 primInfoBufferIndex, u32 materialsBufferIndex)
{
    if (!PassesAlphaTest(attr, primInfoBufferIndex, materialsBufferIndex))
    {
        IgnoreHit();
    }
}

void MissDefault(inout RayPayload payload)
{
}
