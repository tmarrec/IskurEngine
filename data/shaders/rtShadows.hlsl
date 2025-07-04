// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/common.hlsli"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<half> ShadowRenderTarget : register(u0);
Texture2D<float> Depth : register(t1);
SamplerState DepthSampler : register(s0);
cbuffer RtRootConstants : register(b1)
{
    row_major float4x4 invViewProj;

    float zNear;
    float zFar;
    float pad0;
    uint resolutionType;

    float3 sunDir;
    uint frameIndex;

    float3 cameraPos;
    float pad1;
};

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

struct RayPayload
{
    uint hitCount;
};

float3 ReconstructWorldPos(float2 uv)
{
    float d = Depth.SampleLevel(DepthSampler, uv, 0).r;
    float2 ndc0  = uv * 2 - 1;
    ndc0.y = -ndc0.y; // D3D
    float4 clip0   = float4(ndc0, d, 1);
    float4 worldH = mul(clip0, invViewProj);
    return worldH.xyz / worldH.w;
}

void GetDitherOffset(out uint2 fullPxOut, out float2 fullDimInvOut)
{
    uint2 px = DispatchRaysIndex().xy;
    float2 dim = DispatchRaysDimensions().xy;

    uint2 factor = rtFactors[resolutionType];
    uint tileCount = rtTileCount[resolutionType];

    uint slot = frameIndex % tileCount;

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
    uint2 fullPx;
    float2 fullDimInv;
    GetDitherOffset(fullPx, fullDimInv);

    RayPayload payload = { 0 };

    float2 centerUV = (float2(fullPx) + 0.5f) * fullDimInv;
    float3 worldPos = ReconstructWorldPos(centerUV);

    RayDesc ray;
    ray.Origin = worldPos + normalize(cameraPos - worldPos) * 0.025;
    ray.Direction = sunDir;
    ray.TMin = 0.01f;
    ray.TMax = 1e6f;

    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 1, 0, ray, payload);

    ShadowRenderTarget[fullPx] = payload.hitCount;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hitCount++;
}

[shader("miss")] // todo no need?
void Miss(inout RayPayload payload)
{
}