// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/IskurPackFormat.h"

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

class SceneLoader
{
  public:
    struct InstanceRange
    {
        u32 offset; // into GetInstances()
        u32 count;
    };

    enum : u32
    {
        PIPELINE_OPAQUE = 0,
        PIPELINE_MASKED = 1,
        PIPELINE_BLENDED = 2
    };

    static SceneLoader& Get()
    {
        static SceneLoader s;
        return s;
    }

    void Open(const std::filesystem::path& packFile);

    // Look-up by GLOBAL packed-primitive id (CH_PRIM row index)
    const PackedPrimitiveView* GetPrimitiveById(u32 primId) const;

    const Vector<IEPack::TextureRecord>& GetTextureTable() const
    {
        return m_texTable;
    }
    const u8* GetTextureBlobData() const;
    size_t GetTextureBlobSize() const;

    const Vector<IEPack::SamplerDisk>& GetSamplerTable() const
    {
        return m_samplers;
    }

    const Vector<IEPack::MaterialRecord>& GetMaterialTable() const
    {
        return m_materials;
    }

    const Vector<IEPack::InstanceRecord>& GetInstances() const
    {
        return m_instances;
    }

    // --- Draw Lists: six buckets (opaque/masked/blended x culled/no-cull) ---
    // Culled (DRL*/DRI*)
    const Vector<IEPack::DrawItem>& GetDrawItemsCulledOpaque() const
    {
        return m_drl[PIPELINE_OPAQUE];
    }
    const Vector<IEPack::DrawItem>& GetDrawItemsCulledMasked() const
    {
        return m_drl[PIPELINE_MASKED];
    }
    const Vector<IEPack::DrawItem>& GetDrawItemsCulledBlended() const
    {
        return m_drl[PIPELINE_BLENDED];
    }
    const Vector<u32>& GetDrawInstIDsCulledOpaque() const
    {
        return m_dri[PIPELINE_OPAQUE];
    }
    const Vector<u32>& GetDrawInstIDsCulledMasked() const
    {
        return m_dri[PIPELINE_MASKED];
    }
    const Vector<u32>& GetDrawInstIDsCulledBlended() const
    {
        return m_dri[PIPELINE_BLENDED];
    }

    // No-cull (DNL*/DNI*)
    const Vector<IEPack::DrawItem>& GetDrawItemsNoCullOpaque() const
    {
        return m_dnl[PIPELINE_OPAQUE];
    }
    const Vector<IEPack::DrawItem>& GetDrawItemsNoCullMasked() const
    {
        return m_dnl[PIPELINE_MASKED];
    }
    const Vector<IEPack::DrawItem>& GetDrawItemsNoCullBlended() const
    {
        return m_dnl[PIPELINE_BLENDED];
    }
    const Vector<u32>& GetDrawInstIDsNoCullOpaque() const
    {
        return m_dni[PIPELINE_OPAQUE];
    }
    const Vector<u32>& GetDrawInstIDsNoCullMasked() const
    {
        return m_dni[PIPELINE_MASKED];
    }
    const Vector<u32>& GetDrawInstIDsNoCullBlended() const
    {
        return m_dni[PIPELINE_BLENDED];
    }

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

    // Raw file blob + tables
    Vector<IndexEntry> m_index;

    // TXHD/TXTB
    Vector<IEPack::TextureRecord> m_texTable;
    u64 m_texBlobOffset = 0;
    u64 m_texBlobSize = 0;

    // SAMP
    Vector<IEPack::SamplerDisk> m_samplers;

    // MATL
    Vector<IEPack::MaterialRecord> m_materials;

    // INST (CSR over packed-primitive ids)
    Vector<IEPack::InstanceRecord> m_instances; // sorted by primIndex (CSR order)
    Vector<u32> m_instOffsets;                  // size = primCount + 1; [i, i+1) is range for prim i

    // DRL* (culled) + DNL* (no-cull)
    Vector<IEPack::DrawItem> m_drl[3];
    Vector<u32> m_dri[3];
    Vector<IEPack::DrawItem> m_dnl[3];
    Vector<u32> m_dni[3];
};
