// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

groupshared Payload s_Payload;

bool IsVisible(MeshletBounds bounds, float4x4 world, float scale, bool allowBackfaceConeCull)
{
    if (VertexConstants.gpuFrustumCullingEnabled != 0)
    {
        float4 center = mul(float4(bounds.center, 1), world);
        float radius = bounds.radius * scale;
        for (int i = 0; i < 6; ++i)
        {
            if (dot(center, VertexConstants.planes[i]) < -radius)
            {
                return false;
            }
        }
    }

    if (allowBackfaceConeCull && VertexConstants.gpuBackfaceCullingEnabled != 0)
    {
        float3 apexWorld = mul(float4(bounds.cone_apex, 1.0f), world).xyz;
        float3 axisWorld = normalize(mul(bounds.cone_axis, (float3x3)world));
        float3 viewDir = normalize(apexWorld - VertexConstants.cameraPos);
        if (dot(viewDir, axisWorld) >= bounds.cone_cutoff)
        {
            return false;
        }
    }

    return true;
}

[numthreads(32, 1, 1)]
void main(uint dtid : SV_DispatchThreadID)
{
    bool visible = false;
    const bool allowBackfaceConeCull = (Constants.allowBackfaceConeCull != 0);

    if (dtid < Constants.meshletCount)
    {
        StructuredBuffer<MeshletBounds> meshletBoundsBuffer = ResourceDescriptorHeap[Constants.meshletBoundsBufferIndex];
        visible = IsVisible(meshletBoundsBuffer[dtid], Constants.world, Constants.maxWorldScale, allowBackfaceConeCull);
    }

    if (visible)
    {
        uint index = WavePrefixCountBits(visible);
        s_Payload.meshletIndices[index] = dtid;
    }

    uint visibleCount = WaveActiveCountBits(visible);
    DispatchMesh(visibleCount, 1, 1, s_Payload);
}

