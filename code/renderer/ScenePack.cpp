// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "ScenePack.h"

#include "../common/Asserts.h"
#include "CPUGPU.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace
{
#pragma pack(push, 1)
struct PackHeader
{
    char magic[8]; // "ISKURPK"
    u32 version;
    u32 primCount;

    u64 primTableOffset;
    u64 verticesOffset;
    u64 indicesOffset;
    u64 meshletsOffset;
    u64 mlVertsOffset;
    u64 mlTrisOffset;
    u64 mlBoundsOffset;
};

struct PrimRecord
{
    u32 meshIndex;
    u32 primIndex;
    u32 materialIndex;

    u32 vertexCount;
    u32 indexCount;
    u32 meshletCount;

    u64 vertexByteOffset;   // relative to header.verticesOffset
    u64 indexByteOffset;    // relative to header.indicesOffset
    u64 meshletsByteOffset; // relative to header.meshletsOffset
    u64 mlVertsByteOffset;  // relative to header.mlVertsOffset
    u64 mlTrisByteOffset;   // relative to header.mlTrisOffset
    u64 mlBoundsByteOffset; // relative to header.mlBoundsOffset

    u32 mlVertsCount;
    u32 mlTrisByteCount;
};
#pragma pack(pop)
std::unordered_map<u64, u32> s_lookup;
Vector<PackedPrimitiveView> s_views;
} // namespace

void ScenePack::Open(const std::filesystem::path& packFile)
{
    m_blob.Clear();
    m_index.Clear();
    s_views.Clear();
    s_lookup.clear();

    // Read file into memory
    std::ifstream f(packFile, std::ios::binary);
    IE_Assert(f.good());
    f.seekg(0, std::ios::end);
    const std::streamoff fsz = f.tellg();
    IE_Assert(fsz > 0);
    f.seekg(0, std::ios::beg);

    m_blob.Resize(fsz);
    f.read(reinterpret_cast<char*>(m_blob.Data()), fsz);
    IE_Assert(f.good());

    IE_Assert(m_blob.Size() >= sizeof(PackHeader));
    const auto* hdr = reinterpret_cast<const PackHeader*>(m_blob.Data());

    static const char kMagic[8] = {'I', 'S', 'K', 'U', 'R', 'P', 'K', 0};
    IE_Assert(std::memcmp(hdr->magic, kMagic, sizeof(kMagic)) == 0);
    IE_Assert(hdr->version == 2);

    const u64 primCount = hdr->primCount;
    const u64 primTable = hdr->primTableOffset;
    IE_Assert(primTable + primCount * sizeof(PrimRecord) <= m_blob.Size());

    const auto* rec = reinterpret_cast<const PrimRecord*>(m_blob.Data() + primTable);

    m_index.Resize(primCount);
    for (u32 i = 0; i < primCount; ++i)
    {
        const PrimRecord& r = rec[i];
        IndexEntry e{};

        e.meshIndex = r.meshIndex;
        e.primIndex = r.primIndex;
        e.materialIndex = r.materialIndex;

        e.offVertices = hdr->verticesOffset + r.vertexByteOffset;
        e.vertexCount = r.vertexCount;

        e.offIndices = hdr->indicesOffset + r.indexByteOffset;
        e.indexCount = r.indexCount;

        e.offMeshlets = hdr->meshletsOffset + r.meshletsByteOffset;
        e.meshletCount = r.meshletCount;

        e.offMlVerts = hdr->mlVertsOffset + r.mlVertsByteOffset;
        e.mlVertCount = r.mlVertsCount;

        e.offMlTris = hdr->mlTrisOffset + r.mlTrisByteOffset;
        e.mlTriCountBytes = r.mlTrisByteCount;

        e.offMlBounds = hdr->mlBoundsOffset + r.mlBoundsByteOffset;
        e.mlBoundsCount = r.meshletCount;

        m_index[i] = e;
    }

    // Build views + lookup
    s_views.Resize(m_index.Size());
    for (u32 i = 0; i < m_index.Size(); ++i)
    {
        const IndexEntry& e = m_index[i];
        PackedPrimitiveView v{};
        v.materialIndex = e.materialIndex;

        v.vertices = m_blob.Data() + e.offVertices;
        v.vertexCount = e.vertexCount;

        v.indices = m_blob.Data() + e.offIndices;
        v.indexCount = e.indexCount;

        v.meshlets = m_blob.Data() + e.offMeshlets;
        v.meshletCount = e.meshletCount;

        v.mlVerts = m_blob.Data() + e.offMlVerts;
        v.mlVertCount = e.mlVertCount;

        v.mlTris = m_blob.Data() + e.offMlTris;
        v.mlTriCountBytes = e.mlTriCountBytes;

        v.mlBounds = m_blob.Data() + e.offMlBounds;
        v.mlBoundsCount = e.mlBoundsCount;

        s_views[i] = v;
        s_lookup.emplace(static_cast<u64>(e.meshIndex) << 32 | static_cast<u64>(e.primIndex), i);
    }
}

const PackedPrimitiveView* ScenePack::FindPrimitive(i32 meshIndex, i32 primIndex) const
{
    if (meshIndex < 0 || primIndex < 0)
    {
        return nullptr;
    }
    u64 k = (static_cast<u64>(meshIndex) << 32) | static_cast<u64>(primIndex);
    auto it = s_lookup.find(k);
    if (it == s_lookup.end())
    {
        return nullptr;
    }
    u32 idx = it->second;
    IE_Assert(idx < s_views.Size());
    return &s_views[idx];
}
