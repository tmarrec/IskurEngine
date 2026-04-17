// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"
#include "include/geometry/meshlet.hlsli"
#include "include/geometry/normal.hlsli"

ConstantBuffer<VertexConstants> VertexConstants : register(b1);
ConstantBuffer<PrimitiveConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=60, b0), \
                  CBV(b1)"

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
        const uint vertexIndex = GetMeshletVertexIndex(meshletVerticesBuffer, meshletInfo.vertexOffset, gtid);
        verts[gtid] = GetVertexAttributes(verticesBuffer, meshletIndex, vertexIndex, worldSign);
    }
}

