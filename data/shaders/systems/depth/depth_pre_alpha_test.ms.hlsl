// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/geometry/meshlet.hlsli"
#include "include/geometry/normal.hlsli"

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
    float4 color : TEXCOORD0;
    float2 texCoord : TEXCOORD1;
};

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

    ByteAddressBuffer meshletsRaw = ResourceDescriptorHeap[Constants.meshletsBufferIndex];
    ByteAddressBuffer meshletTrianglesBuffer = ResourceDescriptorHeap[Constants.meshletTrianglesBufferIndex];
    StructuredBuffer<uint> meshletVerticesBuffer = ResourceDescriptorHeap[Constants.meshletVerticesBufferIndex];
    StructuredBuffer<Vertex> verticesBuffer = ResourceDescriptorHeap[Constants.verticesBufferIndex];
    MeshletInfo meshletInfo = LoadMeshletInfo(meshletsRaw, meshletIndex);
    const float worldSign = Constants.worldSign;

    SetMeshOutputCounts(meshletInfo.vertexCount, meshletInfo.triangleCount);

    if (gtid < meshletInfo.triangleCount)
    {
        tris[gtid] = ApplyMeshletWinding(GetMeshletTriangle(meshletTrianglesBuffer, meshletInfo.triangleOffset, gtid), worldSign);
    }

    if (gtid < meshletInfo.vertexCount)
    {
        Vertex v = verticesBuffer[GetMeshletVertexIndex(meshletVerticesBuffer, meshletInfo.vertexOffset, gtid)];
        float4 worldPos = mul(float4(v.position, 1.0f), Constants.world);

        VertexOut o;
        o.clipPos = mul(worldPos, VertexConstants.viewProj);
        o.color = DecodePackedColorRGBA16UNORM(v.colorPackedLo, v.colorPackedHi);
        o.texCoord = DecodePackedHalf2(v.texCoordPacked);
        verts[gtid] = o;
    }
}
