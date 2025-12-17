// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=40, b0), \
                  CBV(b1)"

uint3 GetTriangle(uint triangleOffset, uint index)
{
    ByteAddressBuffer meshletTrianglesBuffer = ResourceDescriptorHeap[Constants.meshletTrianglesBufferIndex];

    // Calculate the starting byte offset of the triangle indices
    uint baseOffset = triangleOffset + index * 3;

    // Load the first 32-bit chunk that contains part of the triangle data
    uint chunk0 = meshletTrianglesBuffer.Load(baseOffset & ~3);

    // Load the second 32-bit chunk if necessary (for boundary crossing)
    uint chunk1 = meshletTrianglesBuffer.Load((baseOffset + 4) & ~3);

    // Calculate the starting byte offset within the first chunk
    uint byteOffset = baseOffset & 3;

    // Extract the bytes for the triangle in reversed order
    uint3 tri;
    tri.z = (chunk0 >> (byteOffset * 8)) & 0xFF; // z is the first byte
    tri.y = ((byteOffset + 1) < 4) ? (chunk0 >> ((byteOffset + 1) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 1 - 4) * 8)) & 0xFF;
    tri.x = ((byteOffset + 2) < 4) ? (chunk0 >> ((byteOffset + 2) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 2 - 4) * 8)) & 0xFF;

    return tri;
}

uint GetVertexIndex(uint vertexOffset, uint index)
{
    StructuredBuffer<uint> meshletVerticesBuffer = ResourceDescriptorHeap[Constants.meshletVerticesBufferIndex];
    return meshletVerticesBuffer[(vertexOffset + index)];
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    StructuredBuffer<Vertex> verticesBuffer = ResourceDescriptorHeap[Constants.verticesBufferIndex];
    Vertex v = verticesBuffer[vertexIndex];

    float4 vpos = float4(v.position, 1.0);
    float3x3 W = (float3x3)Constants.world;
    float3x3 WIT = (float3x3)Constants.worldIT;

    float3 Nw = normalize(mul(WIT, v.normal));
    float3 Tt = mul(v.tangent.xyz,  W);
    float3 Tw = normalize(Tt - Nw * dot(Tt, Nw));

    VertexOut o;
    float4 worldPos = mul(vpos, Constants.world);
    o.clipPos = mul(worldPos, VertexConstants.viewProj);
    o.currentClipPosNoJ = mul(worldPos, VertexConstants.viewProjNoJ);
    o.prevClipPosNoJ = mul(worldPos, VertexConstants.prevViewProjNoJ);

    o.N = Nw;
    o.T = Tw;
    o.Tw = v.tangent.w;
    o.normal = Nw;
    o.meshletIndex = meshletIndex;
    o.texCoord = v.texCoord;
    return o;
}

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
    
    SetMeshOutputCounts(vertexCount, triangleCount);
    
    if (gtid < triangleCount)
    {
        tris[gtid] = GetTriangle(triangleOffset, gtid);
    }

    if (gtid < vertexCount)
    {
        const uint vertexIndex = GetVertexIndex(vertexOffset, gtid);
        verts[gtid] = GetVertexAttributes(gid, vertexIndex);
    }
}