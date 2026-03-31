// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/rt/rt_shared.hlsli"
#include "include/geometry/reconstruct_world_pos.hlsli"

ConstantBuffer<RTSpecularConstants> Constants : register(b0);

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    uint2 fullPx = DispatchRaysIndex().xy * Constants.ditherFactors + Constants.ditherOffset;

    uint outW, outH;
    output.GetDimensions(outW, outH);
    if (fullPx.x >= outW || fullPx.y >= outH)
    {
        return;
    }

    float2 uv = ((float2)fullPx + 0.5f) * Constants.fullDimInv;

    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    float depth = depthTex.Load(int3(fullPx, 0)).r;
    if (RTIsSkyDepth(depth))
    {
        output[fullPx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    Texture2D<float2> normalTex = ResourceDescriptorHeap[Constants.normalTextureIndex];
    Texture2D<float2> materialTex = ResourceDescriptorHeap[Constants.materialTextureIndex];

    float3 N = DecodeNormal(normalTex.Load(int3(fullPx, 0)));
    float2 material = materialTex.Load(int3(fullPx, 0));
    float metallic = saturate(material.x);
    float roughness = saturate(material.y);
    float smoothT = 1.0f - roughness;
    smoothT *= smoothT;

    // Match lighting importance: metals and smooth surfaces get most of the RT spec budget.
    const float specTraceMinWeight = 1e-3f;
    float specTraceWeight = metallic * smoothT;
    if (specTraceWeight <= specTraceMinWeight)
    {
        output[fullPx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 P = ReconstructWorldPos(uv, depth, Constants.invViewProj);
    float3 V = normalize(Constants.cameraPos - P);
    float3 R = normalize(reflect(-V, N));

    uint sppLo = min(Constants.sppMin, Constants.sppMax);
    uint sppHi = max(Constants.sppMin, Constants.sppMax);
    uint spp = (uint)round(lerp((float)sppLo, (float)sppHi, smoothT));
    if (spp == 0u)
    {
        output[fullPx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    uint baseSeed = RTHashUint(fullPx.x * 1973u + fullPx.y * 9277u + 89173u + Constants.frameIndex * 26699u);
    float spread = roughness * Constants.roughnessRaySpread;

    float3 outSpec = 0.0f.xxx;
    [loop]
    for (uint s = 0; s < spp; ++s)
    {
        uint seed = baseSeed ^ (s * 0x9E3779B9u);
        float3 rayDir = RTSampleAroundDirection(R, spread, float2(RTRand(seed), RTRand(seed)));
        outSpec += RTEvaluateOneBounceSkySun(
            scene,
            P,
            N,
            rayDir,
            Constants.sunDir,
            Constants.sunColor,
            Constants.sunIntensity,
            Constants.skyCubeIndex,
            Constants.samplerIndex,
            Constants.skyIntensity);
    }
    outSpec *= rcp((float)spp);

    output[fullPx] = float4(outSpec, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hit = 1;

    float t = RayTCurrent();
    float3 ro = WorldRayOrigin();
    float3 rd = WorldRayDirection();
    payload.hitPos = ro + rd * t;
    RTFillPayloadFromTriangleHit(payload, attr, rd, Constants.primInfoBufferIndex, Constants.materialsBufferIndex);
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (!RTPassesAlphaTest(attr, Constants.primInfoBufferIndex, Constants.materialsBufferIndex))
    {
        IgnoreHit();
    }
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
}

