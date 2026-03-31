// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=60, b0), \
                  CBV(b1)"

struct Payload
{
    uint meshletIndices[32];
};

struct VertexOut
{
    float4 clipPos : SV_Position;
};

uint3 GetTriangle(uint triangleOffset, uint index)
{
    ByteAddressBuffer meshletTrianglesBuffer = ResourceDescriptorHeap[Constants.meshletTrianglesBufferIndex];

    uint baseOffset = triangleOffset + index * 3;
    uint chunk0 = meshletTrianglesBuffer.Load(baseOffset & ~3);
    uint chunk1 = meshletTrianglesBuffer.Load((baseOffset + 4) & ~3);
    uint byteOffset = baseOffset & 3;

    uint3 tri;
    tri.z = (chunk0 >> (byteOffset * 8)) & 0xFF;
    tri.y = ((byteOffset + 1) < 4) ? (chunk0 >> ((byteOffset + 1) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 1 - 4) * 8)) & 0xFF;
    tri.x = ((byteOffset + 2) < 4) ? (chunk0 >> ((byteOffset + 2) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 2 - 4) * 8)) & 0xFF;

    return tri;
}

uint GetVertexIndex(uint vertexOffset, uint index)
{
    StructuredBuffer<uint> meshletVerticesBuffer = ResourceDescriptorHeap[Constants.meshletVerticesBufferIndex];
    return meshletVerticesBuffer[vertexOffset + index];
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(126, 1, 1)]
void main(
    in uint gtid : SV_GroupThreadID,
    in uint gid : SV_GroupID,
    in payload Payload payload,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64]
)
{
    uint meshletIndex = payload.meshletIndices[gid];

    if (meshletIndex >= Constants.meshletCount)
    {
        return;
    }

    static const uint MESHLET_BYTE_SIZE = 12;
    ByteAddressBuffer meshletsRaw = ResourceDescriptorHeap[Constants.meshletsBufferIndex];
    uint3 meshletRaw = meshletsRaw.Load3(meshletIndex * MESHLET_BYTE_SIZE);

    uint vertexOffset = meshletRaw.x;
    uint triangleOffset = meshletRaw.y;
    uint vertexCount = meshletRaw.z & 0xFFFF;
    uint triangleCount = meshletRaw.z >> 16;
    const float worldSign = Constants.worldSign;

    SetMeshOutputCounts(vertexCount, triangleCount);

    if (gtid < triangleCount)
    {
        uint3 tri = GetTriangle(triangleOffset, gtid);
        if (worldSign < 0.0f)
        {
            const uint tmp = tri.x;
            tri.x = tri.y;
            tri.y = tmp;
        }
        tris[gtid] = tri;
    }

    if (gtid < vertexCount)
    {
        StructuredBuffer<Vertex> verticesBuffer = ResourceDescriptorHeap[Constants.verticesBufferIndex];
        Vertex v = verticesBuffer[GetVertexIndex(vertexOffset, gtid)];
        float4 worldPos = mul(float4(v.position, 1.0f), Constants.world);

        VertexOut o;
        o.clipPos = mul(worldPos, VertexConstants.viewProj);
        verts[gtid] = o;
    }
}
