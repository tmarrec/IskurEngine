// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/asCommon.hlsli"

groupshared Payload s_Payload;

[NumThreads(32, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint dtid : SV_DispatchThreadID, uint gid : SV_GroupID)
{
    bool visible = false;

    // Check bounds of meshlet cull data resource
    if (dtid < PrimitiveConstants.meshletCount)
    {
        float scaleX = length(PrimitiveConstants.world[0].xyz);
        StructuredBuffer<MeshletBounds> meshletBoundsBuffer = ResourceDescriptorHeap[PrimitiveConstants.meshletBoundsBufferIndex];
        visible = IsVisible(meshletBoundsBuffer[dtid], PrimitiveConstants.world, scaleX, Globals.cameraPos);
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