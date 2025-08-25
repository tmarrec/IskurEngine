// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <filesystem>

struct PackedPrimitiveView
{
    u32 materialIndex;

    const void* vertices;
    u32 vertexCount;
    const void* indices;
    u32 indexCount;

    const void* meshlets;
    u32 meshletCount;
    const void* mlVerts;
    u32 mlVertCount;
    const void* mlTris;
    u32 mlTriCountBytes;

    const void* mlBounds;
    u32 mlBoundsCount;
};

class ScenePack
{
  public:
    static ScenePack& Get()
    {
        static ScenePack s;
        return s;
    }

    void Open(const std::filesystem::path& packFile);
    const PackedPrimitiveView* FindPrimitive(i32 meshIndex, i32 primIndex) const;

  private:
    struct IndexEntry
    {
        u32 meshIndex;
        u32 primIndex;
        u32 materialIndex;

        u64 offVertices;
        u32 vertexCount;
        u64 offIndices;
        u32 indexCount;
        u64 offMeshlets;
        u32 meshletCount;
        u64 offMlVerts;
        u32 mlVertCount;
        u64 offMlTris;
        u32 mlTriCountBytes;

        u64 offMlBounds;
        u32 mlBoundsCount;
    };

    Vector<u8> m_blob;
    Vector<IndexEntry> m_index;
};
