// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/Common.hlsli"
#include "data/CPUGPU.h"
#include "data/shaders/utils/ReconstructWorldPos.hlsli"

ConstantBuffer<RtShadowsTraceConstants> Constants : register(b0);

static const uint invBayer2[4] = { 0, 3, 1, 2 };
static const uint invBayer4[16] = { 0,10,2,8, 5,15,7,13, 1,11,3,9, 4,14,6,12 };

static const uint2 rtFactors[4] =
{
    uint2(1, 1),   // full
    uint2(1, 2),   // fullX_halfY
    uint2(2, 2),   // half
    uint2(4, 4)    // quarter
};
static const uint rtTileCount[4] =
{
    1,  // full
    2,  // fullX_halfY
    4,  // half
    16  // quarter
};

struct [raypayload] RayPayload
{
    uint hitCount : read(caller, closesthit) : write(caller, closesthit);
};

void GetDitherOffset(out uint2 fullPxOut, out float2 fullDimInvOut)
{
    uint2 px = DispatchRaysIndex().xy;
    float2 dim = DispatchRaysDimensions().xy;

    uint2 factor = rtFactors[Constants.resolutionType];
    uint tileCount = rtTileCount[Constants.resolutionType];

    uint slot = Constants.frameIndex % tileCount;

    uint idx;
    if (tileCount < 4)
    {
        idx = slot;
    }
    else if (tileCount == 4)
    {
        idx = invBayer2[slot];
    }
    else
    {
        idx = invBayer4[slot];
    }

    uint shift = factor.x >> 1;  
    uint mask = factor.x - 1;

    uint2 ditherOffset = uint2(idx & mask, idx >> shift);

    fullPxOut = px * factor + ditherOffset;
    fullDimInvOut = 1.f / (dim * factor); 
}

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<half> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    uint2 fullPx;
    float2 fullDimInv;
    GetDitherOffset(fullPx, fullDimInv);

    RayPayload payload = { 0 };

    float2 centerUV = (float2(fullPx) + 0.5f) * fullDimInv;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[Constants.depthTextureIndex];
    SamplerState depthSampler = SamplerDescriptorHeap[Constants.depthSamplerIndex];
    float depth = depthTexture.Load(int3(fullPx, 0)).r;
    float3 worldPos = ReconstructWorldPos(centerUV, depth, Constants.invViewProj);

    RayDesc ray;
    ray.Origin = worldPos + normalize(Constants.cameraPos - worldPos) * 0.025;
    ray.Direction = -Constants.sunDir;
    ray.TMin = 0.01f;
    ray.TMax = 1e6f;

    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 1, 0, ray, payload);

    output[fullPx] = half(payload.hitCount);
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hitCount++;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    // intentionally empty
}