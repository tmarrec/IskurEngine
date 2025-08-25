// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/Common.hlsli"
#include "data/CPUGPU.h"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

groupshared Payload s_Payload;

bool IsVisible(MeshletBounds bounds, float4x4 world, float scale, float3 viewPos)
{
    // Frustum culling
    float4 center = mul(float4(bounds.center, 1), world);
    float radius = bounds.radius * scale;
    for (int i = 0; i < 6; ++i)
    {
        if (dot(center, VertexConstants.planes[i]) < -radius)
        {
            return false;
        }
    }
    
    /*
    // Backface culling
    if (dot(normalize(bounds.cone_apex - viewPos), bounds.cone_axis) >= bounds.cone_cutoff)
    {
        return false;
    }
    */
    
    return true;
}

[NumThreads(32, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint dtid : SV_DispatchThreadID, uint gid : SV_GroupID)
{
    bool visible = false;

    // Check bounds of meshlet cull data resource
    if (dtid < Constants.meshletCount)
    {
        float scaleX = length(Constants.world[0].xyz);
        StructuredBuffer<MeshletBounds> meshletBoundsBuffer = ResourceDescriptorHeap[Constants.meshletBoundsBufferIndex];
        visible = IsVisible(meshletBoundsBuffer[dtid], Constants.world, scaleX, VertexConstants.cameraPos);
    }

    // Compact visible meshlets into the export payload array
    if (visible)
    {
        uint index = WavePrefixCountBits(visible);
        s_Payload.meshletIndices[index] = dtid;
    }
	
    // Dispatch the required number of MS threadgroups to render the visible meshlets
    uint visibleCount = WaveActiveCountBits(visible);
    DispatchMesh(visibleCount, 1, 1, s_Payload);
}