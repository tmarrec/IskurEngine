// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"
#include "include/geometry/normal.hlsli"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=60, b0), \
                  CBV(b1)"

uint3 GetTriangle(ByteAddressBuffer meshletTrianglesBuffer, uint triangleOffset, uint index)
{
    uint baseOffset = triangleOffset + index * 3;
    uint chunk0 = meshletTrianglesBuffer.Load(baseOffset & ~3);
    uint chunk1 = meshletTrianglesBuffer.Load((baseOffset + 4) & ~3);
    uint byteOffset = baseOffset & 3;
    uint3 tri;
    tri.z = (chunk0 >> (byteOffset * 8)) & 0xFF; // z is the first byte
    tri.y = ((byteOffset + 1) < 4) ? (chunk0 >> ((byteOffset + 1) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 1 - 4) * 8)) & 0xFF;
    tri.x = ((byteOffset + 2) < 4) ? (chunk0 >> ((byteOffset + 2) * 8)) & 0xFF : (chunk1 >> ((byteOffset + 2 - 4) * 8)) & 0xFF;

    return tri;
}

uint GetVertexIndex(StructuredBuffer<uint> meshletVerticesBuffer, uint vertexOffset, uint index)
{
    return meshletVerticesBuffer[(vertexOffset + index)];
}

VertexOut GetVertexAttributes(StructuredBuffer<Vertex> verticesBuffer, uint meshletIndex, uint vertexIndex, float worldSign)
{
    Vertex v = verticesBuffer[vertexIndex];

    float4 vpos = float4(v.position, 1.0);
    float3x3 W = (float3x3)Constants.world;
    float3x3 WInv = (float3x3)Constants.worldInv;

    float3 Nw = normalize(mul(WInv, DecodePackedNormalOct(v.normalPacked)));
    float tangentSign = 1.0f;
    float3 Tobj = DecodePackedTangentR10G10B10A2(v.tangentPacked, tangentSign);
    float3 Tt = mul(Tobj, W);
    float3 Tw = normalize(Tt - Nw * dot(Tt, Nw));
    VertexOut o;
    float4 worldPos = mul(vpos, Constants.world);
    float4 prevWorldPos = mul(vpos, Constants.prevWorld);
    float4 currentClipPosNoJ = mul(worldPos, VertexConstants.viewProjNoJ);
    float4 prevClipPosNoJ = mul(prevWorldPos, VertexConstants.prevViewProjNoJ);
    o.clipPos = mul(worldPos, VertexConstants.viewProj);
    o.currentClipPosNoJ = currentClipPosNoJ.xyw;
    o.prevClipPosNoJ = prevClipPosNoJ.xyw;
    o.color = DecodePackedColorRGBA16UNORM(v.colorPackedLo, v.colorPackedHi);

    o.N = Nw;
    o.T = Tw;
    o.Tw = tangentSign * worldSign;
    o.meshletIndex = meshletIndex;
    o.texCoord = DecodePackedHalf2(v.texCoordPacked);
    return o;
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
    ByteAddressBuffer meshletTrianglesBuffer = ResourceDescriptorHeap[Constants.meshletTrianglesBufferIndex];
    StructuredBuffer<uint> meshletVerticesBuffer = ResourceDescriptorHeap[Constants.meshletVerticesBufferIndex];
    StructuredBuffer<Vertex> verticesBuffer = ResourceDescriptorHeap[Constants.verticesBufferIndex];
    uint3 meshletRaw = meshletsRaw.Load3(meshletIndex * MESHLET_BYTE_SIZE);

    uint vertexOffset = meshletRaw.x;
    uint triangleOffset = meshletRaw.y;
    uint vertexCount = meshletRaw.z & 0xFFFF;
    uint triangleCount = meshletRaw.z >> 16;
    const float worldSign = Constants.worldSign;
    
    SetMeshOutputCounts(vertexCount, triangleCount);
    
    if (gtid < triangleCount)
    {
        uint3 tri = GetTriangle(meshletTrianglesBuffer, triangleOffset, gtid);
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
        const uint vertexIndex = GetVertexIndex(meshletVerticesBuffer, vertexOffset, gtid);
        verts[gtid] = GetVertexAttributes(verticesBuffer, meshletIndex, vertexIndex, worldSign);
    }
}

