// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/msCommon.hlsli"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=40, b0), \
                  CBV(b1)"

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[NumThreads(124, 1, 1)]
void main(
	in uint gtid : SV_GroupThreadID,
    in uint gid : SV_GroupID,
	in payload Payload payload,
    out indices uint3 tris[124],
    out vertices VertexOut verts[64]
)
{
    uint meshletIndex = payload.meshletIndices[gid];

    if (meshletIndex >= PrimitiveConstants.meshletCount)
    {
        return;
    }
    
    StructuredBuffer<Meshlet> meshletsBuffer = ResourceDescriptorHeap[PrimitiveConstants.meshletsBufferIndex];
    Meshlet m = meshletsBuffer[meshletIndex];

    SetMeshOutputCounts(m.vertexCount, m.triangleCount);
    
    if (gtid < m.triangleCount)
    {
        tris[gtid] = GetTriangle(m, gtid);
    }

    if (gtid < m.vertexCount)
    {
        const uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid] = GetVertexAttributes(gid, vertexIndex);
    }
}