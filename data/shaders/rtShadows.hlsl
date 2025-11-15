// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/Common.hlsli"
#include "data/CPUGPU.h"
#include "data/shaders/utils/ReconstructWorldPos.hlsli"

ConstantBuffer<RtShadowsTraceConstants> Constants : register(b0);

struct [raypayload] RayPayload
{
    uint shadowed : read(caller, anyhit) : write(caller, anyhit);
};

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<half> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    // Dithered pixel
    uint2 fullPx = DispatchRaysIndex().xy * Constants.ditherFactors + Constants.ditherOffset;

    float2 centerUV = (float2(fullPx) + 0.5f) * Constants.fullDimInv;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[Constants.depthTextureIndex];
    float depth = depthTexture.Load(int3(fullPx, 0)).r;
    float3 worldPos = ReconstructWorldPos(centerUV, depth, Constants.invViewProj);

    RayPayload payload;
    payload.shadowed = 0;

    RayDesc ray;
    ray.Origin = worldPos;
    ray.Direction = -Constants.sunDir;
    ray.TMin = 0.025f;
    ray.TMax = 1e6f;

    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 0, 1, 0, ray, payload);

    output[fullPx] = half(payload.shadowed);
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.shadowed = 1;
    AcceptHitAndEndSearch();
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    // intentionally empty
}