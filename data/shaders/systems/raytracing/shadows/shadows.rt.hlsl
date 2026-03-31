// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "include/rt/rt_shared.hlsli"
#include "include/geometry/reconstruct_world_pos.hlsli"

ConstantBuffer<RTShadowsTraceConstants> Constants : register(b0);

static const float SHADOW_RAY_TMIN = 1e-4f;
static const float SHADOW_RAY_MIN_BIAS = 1e-3f;
static const float SHADOW_RAY_VIEW_BIAS_SCALE = 3e-5f;

struct [raypayload] ShadowRayPayload
{
    uint shadowed : read(caller, anyhit, closesthit) : write(caller, anyhit, closesthit);
};

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<half> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    uint2 fullPx = DispatchRaysIndex().xy * Constants.ditherFactors + Constants.ditherOffset;

    uint outW, outH;
    output.GetDimensions(outW, outH);
    if (fullPx.x >= outW || fullPx.y >= outH)
    {
        return;
    }

    float2 centerUV = (float2(fullPx) + 0.5f) * Constants.fullDimInv;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[Constants.depthTextureIndex];
    float depth = depthTexture.Load(int3(fullPx, 0)).r;

    // No geometry at this pixel -> no shadow ray needed.
    if (depth <= 1e-6f)
    {
        output[fullPx] = half(0.0f);
        return;
    }

    float3 worldPos = ReconstructWorldPos(centerUV, depth, Constants.invViewProj);
    Texture2D<float2> normalGeoTexture = ResourceDescriptorHeap[Constants.normalGeoTextureIndex];
    float3 geoN = DecodeNormal(normalGeoTexture.Load(int3(fullPx, 0)));
    float3 sunL = normalize(-Constants.sunDir);
    if (dot(geoN, sunL) < 0.0f)
    {
        geoN = -geoN;
    }

    float viewDist = length(worldPos - Constants.cameraPos);
    float baseBias = max(SHADOW_RAY_MIN_BIAS, SHADOW_RAY_VIEW_BIAS_SCALE * viewDist);
    float slopeScale = 1.0f + 2.0f * (1.0f - saturate(dot(geoN, sunL)));
    float originBias = baseBias * slopeScale;

    ShadowRayPayload payload;
    payload.shadowed = 0;

    RayDesc ray;
    ray.Origin = worldPos + geoN * originBias;
    ray.Direction = sunL;
    ray.TMin = SHADOW_RAY_TMIN;
    ray.TMax = 1e6f;

    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 1, 0, ray, payload);

    output[fullPx] = half(payload.shadowed);
}

[shader("closesthit")]
void ClosestHit(inout ShadowRayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.shadowed = 1;
}

[shader("anyhit")]
void AnyHit(inout ShadowRayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (!RTPassesAlphaTest(attr, Constants.primInfoBufferIndex, Constants.materialsBufferIndex))
    {
        IgnoreHit();
        return;
    }

    payload.shadowed = 1;
    AcceptHitAndEndSearch();
}

[shader("miss")]
void Miss(inout ShadowRayPayload payload)
{
    // intentionally empty
}

