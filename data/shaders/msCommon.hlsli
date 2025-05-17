// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/common.hlsli"

uint3 GetTriangle(Meshlet m, uint index)
{
    ByteAddressBuffer meshletTrianglesBuffer = ResourceDescriptorHeap[PrimitiveConstants.meshletTrianglesBufferIndex];

    // Calculate the starting byte offset of the triangle indices
    uint baseOffset = m.triangleOffset + index * 3;

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

uint GetVertexIndex(Meshlet m, uint index)
{
    StructuredBuffer<uint> meshletVerticesBuffer = ResourceDescriptorHeap[PrimitiveConstants.meshletVerticesBufferIndex];
    return meshletVerticesBuffer[(m.vertexOffset + index)];
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    StructuredBuffer<Vertex> verticesBuffer = ResourceDescriptorHeap[PrimitiveConstants.verticesBufferIndex];

    Vertex v = verticesBuffer[vertexIndex];
    float4 vpos = float4(v.position, 1);

    VertexOut vout;
    vout.posWorld = mul(vpos, PrimitiveConstants.world).xyz;
    vout.posWorldView = mul(vpos, mul(PrimitiveConstants.world, Globals.view)).xyz;
    vout.posWorldViewProj = mul(vpos, mul(mul(PrimitiveConstants.world, Globals.view), Globals.proj));
    vout.normal = mul(float4(v.normal, 0), PrimitiveConstants.world).xyz;
    vout.meshletIndex = meshletIndex;
    vout.texCoord = v.texCoord;

    float3 normalWorld = normalize(mul(float4(v.normal,  0), PrimitiveConstants.world).xyz);
    float3 tangentWorld = normalize(mul(float4(v.tangent.xyz, 0), PrimitiveConstants.world).xyz);
    float3 bitangentWorld = normalize(cross(normalWorld, tangentWorld) * v.tangent.w);
    vout.TBN = float3x3(tangentWorld, bitangentWorld, normalWorld);

    return vout;
}