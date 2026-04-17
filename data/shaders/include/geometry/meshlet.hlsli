// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

static const uint kMeshletByteSize = 12;

struct MeshletInfo
{
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

MeshletInfo LoadMeshletInfo(ByteAddressBuffer meshletsRaw, uint meshletIndex)
{
    uint3 meshletRaw = meshletsRaw.Load3(meshletIndex * kMeshletByteSize);

    MeshletInfo meshletInfo;
    meshletInfo.vertexOffset = meshletRaw.x;
    meshletInfo.triangleOffset = meshletRaw.y;
    meshletInfo.vertexCount = meshletRaw.z & 0xFFFF;
    meshletInfo.triangleCount = meshletRaw.z >> 16;
    return meshletInfo;
}

uint3 GetMeshletTriangle(ByteAddressBuffer meshletTrianglesBuffer, uint triangleOffset, uint index)
{
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

uint GetMeshletVertexIndex(StructuredBuffer<uint> meshletVerticesBuffer, uint vertexOffset, uint index)
{
    return meshletVerticesBuffer[vertexOffset + index];
}

uint3 ApplyMeshletWinding(uint3 tri, float worldSign)
{
    if (worldSign < 0.0f)
    {
        const uint tmp = tri.x;
        tri.x = tri.y;
        tri.y = tmp;
    }

    return tri;
}
