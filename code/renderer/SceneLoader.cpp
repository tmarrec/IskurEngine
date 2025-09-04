// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneLoader.h"
#include "CPUGPU.h"
#include "common/Asserts.h"
#include "common/IskurPackFormat.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <execution>
#include <numeric>
#include <unordered_map>
#include <vector>

#define NOMINMAX
#include <windows.h>

namespace
{
using namespace IEPack;

struct Bases
{
    u64 prim = 0, vert = 0, indx = 0, mshl = 0, mlvt = 0, mltr = 0, mlbd = 0;
    u64 txhd = 0, txtb = 0, txtbSize = 0;
};

bool magic7_ok(const char magic[8])
{
    static const char mk[7] = {'I', 'S', 'K', 'U', 'R', 'P', 'K'};
    return std::memcmp(magic, mk, 7) == 0;
}

// Lookup from original (meshIndex, primIndex) -> global packed-prim id
std::unordered_map<u64, u32> s_lookup;
// Array of packed views indexed by global packed-prim id
Vector<PackedPrimitiveView> s_views;

Vector<u32> make_index(u32 n)
{
    Vector<u32> idx(n);
    std::iota(idx.begin(), idx.end(), 0u);
    return idx;
}

// ---- Windows file mapping kept alive across loads ----
struct MappedFile
{
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE mh = nullptr;
    void* view = nullptr;
    size_t size = 0;

    void close()
    {
        if (view)
        {
            UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mh)
        {
            CloseHandle(mh);
            mh = nullptr;
        }
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        size = 0;
    }

    ~MappedFile()
    {
        close();
    }

    // non-copyable
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // movable
    MappedFile() = default;
    MappedFile(MappedFile&& o) noexcept
    {
        *this = std::move(o);
    }
    MappedFile& operator=(MappedFile&& o) noexcept
    {
        if (this != &o)
        {
            close();
            h = o.h;
            mh = o.mh;
            view = o.view;
            size = o.size;
            o.h = INVALID_HANDLE_VALUE;
            o.mh = nullptr;
            o.view = nullptr;
            o.size = 0;
        }
        return *this;
    }

    void open(const std::filesystem::path& p)
    {
        close();
        h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        IE_Assert(h != INVALID_HANDLE_VALUE);

        LARGE_INTEGER sz{};
        IE_Assert(GetFileSizeEx(h, &sz));
        IE_Assert(sz.QuadPart > 0);
        size = static_cast<size_t>(sz.QuadPart);

        mh = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        IE_Assert(mh != nullptr);

        view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
        IE_Assert(view != nullptr);
    }
};

MappedFile g_mapped;

} // namespace

void SceneLoader::Open(const std::filesystem::path& packFile)
{
    g_mapped.open(packFile);
    const u8* const blob = static_cast<const u8*>(g_mapped.view);
    const u64 blobSize = static_cast<u64>(g_mapped.size);
    IE_Assert(blob && blobSize > 0);

    const auto* hdr = reinterpret_cast<const PackHeaderV5*>(blob);
    IE_Assert(magic7_ok(hdr->magic));
    IE_Assert(hdr->version == 5);

    const auto* chunks = reinterpret_cast<const ChunkRecord*>(blob + hdr->chunkTableOffset);

    auto findChunk = [&](u32 id) -> const ChunkRecord* {
        for (u32 i = 0; i < hdr->chunkCount; ++i)
            if (chunks[i].id == id)
                return &chunks[i];
        return nullptr;
    };

    // Required geometry chunks
    const ChunkRecord* cPRIM = findChunk(CH_PRIM);
    const ChunkRecord* cVERT = findChunk(CH_VERT);
    const ChunkRecord* cINDX = findChunk(CH_INDX);
    const ChunkRecord* cMSHL = findChunk(CH_MSHL);
    const ChunkRecord* cMLVT = findChunk(CH_MLVT);
    const ChunkRecord* cMLTR = findChunk(CH_MLTR);
    const ChunkRecord* cMLBD = findChunk(CH_MLBD);

    IE_Assert(cPRIM && cVERT && cINDX && cMSHL && cMLVT && cMLTR && cMLBD && "Missing required mesh chunks");

    // Optional chunks
    const ChunkRecord* cTXHD = findChunk(CH_TXHD);
    const ChunkRecord* cTXTB = findChunk(CH_TXTB);
    const ChunkRecord* cSAMP = findChunk(CH_SAMP);
    const ChunkRecord* cMATL = findChunk(CH_MATL);
    const ChunkRecord* cINST = findChunk(CH_INST);

    // Draw list chunks (culled / no-cull) — optional
    const ChunkRecord* cDRL[3] = {findChunk(CH_DRL0), findChunk(CH_DRL1), findChunk(CH_DRL2)};
    const ChunkRecord* cDRI[3] = {findChunk(CH_DRI0), findChunk(CH_DRI1), findChunk(CH_DRI2)};
    const ChunkRecord* cDNL[3] = {findChunk(CH_DNL0), findChunk(CH_DNL1), findChunk(CH_DNL2)};
    const ChunkRecord* cDNI[3] = {findChunk(CH_DNI0), findChunk(CH_DNI1), findChunk(CH_DNI2)};

    auto ck = [&](const ChunkRecord* c) {
        if (c)
            IE_Assert(c->offset + c->size <= blobSize);
    };
    ck(cPRIM);
    ck(cVERT);
    ck(cINDX);
    ck(cMSHL);
    ck(cMLVT);
    ck(cMLTR);
    ck(cMLBD);
    ck(cTXHD);
    ck(cTXTB);
    ck(cSAMP);
    ck(cMATL);
    ck(cINST);
    for (int p = 0; p < 3; ++p)
    {
        ck(cDRL[p]);
        ck(cDRI[p]);
        ck(cDNL[p]);
        ck(cDNI[p]);
    }

    Bases b{};
    b.prim = cPRIM->offset;
    b.vert = cVERT->offset;
    b.indx = cINDX->offset;
    b.mshl = cMSHL->offset;
    b.mlvt = cMLVT->offset;
    b.mltr = cMLTR->offset;
    b.mlbd = cMLBD->offset;

    // --- Texture table (optional) ---
    if (cTXHD && cTXTB)
    {
        IE_Assert(cTXHD->size % sizeof(TextureRecord) == 0);
        const u32 txCount = static_cast<u32>(cTXHD->size / sizeof(TextureRecord));
        m_texTable.resize(txCount);

        b.txhd = cTXHD->offset;
        b.txtb = cTXTB->offset;
        b.txtbSize = cTXTB->size;

        const auto* txDisk = reinterpret_cast<const TextureRecord*>(blob + b.txhd);

        if (txCount)
        {
            auto I = make_index(txCount);
            std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
                TextureRecord r{};
                r.imageIndex = txDisk[i].imageIndex;
                r.flags = txDisk[i].flags;
                r.byteOffset = txDisk[i].byteOffset;
                r.byteSize = txDisk[i].byteSize;
                IE_Assert(r.byteOffset + r.byteSize <= b.txtbSize);
                m_texTable[i] = r;
            });
        }

        // IMPORTANT: base pointer for textures is the start of CH_TXTB
        m_texBlobOffset = b.txtb;
        m_texBlobSize = b.txtbSize;
    }
    else
    {
        m_texTable.clear();
        m_texBlobOffset = 0;
        m_texBlobSize = 0;
    }

    // --- Samplers (optional) ---
    if (cSAMP)
    {
        IE_Assert(cSAMP->size % sizeof(SamplerDisk) == 0);
        const u32 sampCount = static_cast<u32>(cSAMP->size / sizeof(SamplerDisk));
        m_samplers.resize(sampCount);

        const auto* sd = reinterpret_cast<const SamplerDisk*>(blob + cSAMP->offset);
        if (sampCount)
        {
            auto I = make_index(sampCount);
            std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
                SamplerDisk r{};
                r.d3d12Filter = sd[i].d3d12Filter;
                r.addressU = sd[i].addressU;
                r.addressV = sd[i].addressV;
                r.addressW = sd[i].addressW;
                r.mipLODBias = sd[i].mipLODBias;
                r.minLOD = sd[i].minLOD;
                r.maxLOD = sd[i].maxLOD;
                r.maxAnisotropy = sd[i].maxAnisotropy;
                r.comparisonFunc = sd[i].comparisonFunc;
                r.borderColor[0] = sd[i].borderColor[0];
                r.borderColor[1] = sd[i].borderColor[1];
                r.borderColor[2] = sd[i].borderColor[2];
                r.borderColor[3] = sd[i].borderColor[3];
                m_samplers[i] = r;
            });
        }
    }

    // --- Materials (optional) ---
    if (cMATL)
    {
        IE_Assert(cMATL->size % sizeof(MaterialRecord) == 0);
        const u32 matCount = static_cast<u32>(cMATL->size / sizeof(MaterialRecord));
        m_materials.resize(matCount);

        const auto* md = reinterpret_cast<const MaterialRecord*>(blob + cMATL->offset);
        if (matCount)
        {
            auto I = make_index(matCount);
            std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
                MaterialRecord r{};
                r.baseColorTx = md[i].baseColorTx;
                r.normalTx = md[i].normalTx;
                r.metallicRoughTx = md[i].metallicRoughTx;
                r.occlusionTx = md[i].occlusionTx;
                r.emissiveTx = md[i].emissiveTx;

                r.baseColorSampler = md[i].baseColorSampler;
                r.normalSampler = md[i].normalSampler;
                r.metallicRoughSampler = md[i].metallicRoughSampler;
                r.occlusionSampler = md[i].occlusionSampler;
                r.emissiveSampler = md[i].emissiveSampler;

                r.baseColorFactor[0] = md[i].baseColorFactor[0];
                r.baseColorFactor[1] = md[i].baseColorFactor[1];
                r.baseColorFactor[2] = md[i].baseColorFactor[2];
                r.baseColorFactor[3] = md[i].baseColorFactor[3];

                r.emissiveFactor[0] = md[i].emissiveFactor[0];
                r.emissiveFactor[1] = md[i].emissiveFactor[1];
                r.emissiveFactor[2] = md[i].emissiveFactor[2];

                r.metallicFactor = md[i].metallicFactor;
                r.roughnessFactor = md[i].roughnessFactor;
                r.normalScale = md[i].normalScale;
                r.occlusionStrength = md[i].occlusionStrength;
                r.alphaCutoff = md[i].alphaCutoff;
                r.flags = md[i].flags;

                r.uvScale[0] = md[i].uvScale[0];
                r.uvScale[1] = md[i].uvScale[1];
                r.uvOffset[0] = md[i].uvOffset[0];
                r.uvOffset[1] = md[i].uvOffset[1];
                r.uvRotation = md[i].uvRotation;
                r._pad1 = md[i]._pad1;

                m_materials[i] = r;
            });
        }
    }

    // --- Mesh prims (build before INST) ---
    IE_Assert(cPRIM->size >= u64(hdr->primCount) * sizeof(PrimRecord));
    const auto* prim = reinterpret_cast<const PrimRecord*>(blob + b.prim);

    m_index.resize(hdr->primCount);
    if (hdr->primCount)
    {
        auto I = make_index(hdr->primCount);
        std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
            const PrimRecord& r = prim[i];
            IndexEntry e{};
            e.meshIndex = r.meshIndex;
            e.primIndex = r.primIndex;
            e.materialIndex = r.materialIndex;

            e.offVertices = b.vert + r.vertexByteOffset;
            e.vertexCount = r.vertexCount;

            e.offIndices = b.indx + r.indexByteOffset;
            e.indexCount = r.indexCount;

            e.offMeshlets = b.mshl + r.meshletsByteOffset;
            e.meshletCount = r.meshletCount;

            e.offMlVerts = b.mlvt + r.mlVertsByteOffset;
            e.mlVertCount = r.mlVertsCount;

            e.offMlTris = b.mltr + r.mlTrisByteOffset;
            e.mlTriCountBytes = r.mlTrisByteCount;

            e.offMlBounds = b.mlbd + r.mlBoundsByteOffset;
            e.mlBoundsCount = r.meshletCount;

#ifndef NDEBUG
            IE_Assert(e.offVertices + u64(e.vertexCount) * sizeof(Vertex) <= blobSize);
            IE_Assert(e.offIndices + u64(e.indexCount) * sizeof(u32) <= blobSize);
            IE_Assert(e.offMeshlets + u64(e.meshletCount) * sizeof(Meshlet) <= blobSize);
            IE_Assert(e.offMlVerts + u64(e.mlVertCount) * sizeof(u32) <= blobSize);
            IE_Assert(e.offMlTris + u64(e.mlTriCountBytes) <= blobSize);
            IE_Assert(e.offMlBounds + u64(e.mlBoundsCount) * sizeof(MeshletBounds) <= blobSize);
#endif
            m_index[i] = e;
        });
    }

    // Views + lookup
    s_views.resize(m_index.size());
    if (m_index.size())
    {
        auto I = make_index((u32)m_index.size());
        std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
            const auto& e = m_index[i];
            PackedPrimitiveView v{};
            v.materialIndex = e.materialIndex;

            v.vertices = blob + e.offVertices;
            v.vertexCount = e.vertexCount;

            v.indices = blob + e.offIndices;
            v.indexCount = e.indexCount;

            v.meshlets = blob + e.offMeshlets;
            v.meshletCount = e.meshletCount;

            v.mlVerts = blob + e.offMlVerts;
            v.mlVertCount = e.mlVertCount;

            v.mlTris = blob + e.offMlTris;
            v.mlTriCountBytes = e.mlTriCountBytes;

            v.mlBounds = blob + e.offMlBounds;
            v.mlBoundsCount = e.mlBoundsCount;

            s_views[i] = v;
        });

        // fill unordered_map sequentially
        s_lookup.clear();
        s_lookup.reserve((size_t)m_index.size() * 2u);
        for (u32 i = 0; i < m_index.size(); ++i)
        {
            const auto& e = m_index[i];
            s_lookup.emplace((u64(e.meshIndex) << 32) | u64(e.primIndex), i);
        }
    }

    // --- Instances (optional; CSR over global prim ids) ---
    if (cINST)
    {
        IE_Assert(cINST->size % sizeof(InstanceRecord) == 0);
        const u32 instCount = static_cast<u32>(cINST->size / sizeof(InstanceRecord));
        const auto* id = reinterpret_cast<const InstanceRecord*>(blob + cINST->offset);

        const u32 primCount = static_cast<u32>(m_index.size());
        IE_Assert(primCount > 0);

        // Count per-prim
        Vector<u32> counts;
        counts.resize(primCount);
        for (u32 p = 0; p < primCount; ++p)
            counts[p] = 0;
        for (u32 i = 0; i < instCount; ++i)
        {
            IE_Assert(id[i].primIndex < primCount && "INST.primIndex must be global packed-prim id");
            ++counts[id[i].primIndex];
        }

        // Prefix sum -> offsets
        m_instOffsets.resize(primCount + 1);
        u32 run = 0;
        for (u32 p = 0; p < primCount; ++p)
        {
            m_instOffsets[p] = run;
            run += counts[p];
        }
        m_instOffsets[primCount] = run;
        IE_Assert(run == instCount);

        // Place instances into CSR-sorted buffer (parallel scatter with atomic cursors)
        m_instances.resize(instCount);
        Vector<std::atomic<u32>> cursor(primCount);
        for (u32 p = 0; p < primCount; ++p)
            cursor[p].store(m_instOffsets[p], std::memory_order_relaxed);

        if (instCount)
        {
            auto I = make_index(instCount);
            std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
                const u32 p = id[i].primIndex;
                u32 dst = cursor[p].fetch_add(1, std::memory_order_relaxed);

                InstanceRecord r{};
                r.primIndex = id[i].primIndex; // already global
                r.materialOverride = id[i].materialOverride;
                r.world = id[i].world;
                std::memcpy(r.aabbMin, id[i].aabbMin, sizeof(r.aabbMin));
                std::memcpy(r.aabbMax, id[i].aabbMax, sizeof(r.aabbMax));
                std::memcpy(r.bsCenter, id[i].bsCenter, sizeof(r.bsCenter));
                r.bsRadius = id[i].bsRadius;
                r.maxScale = id[i].maxScale;

                m_instances[dst] = r;
            });
        }
    }

    // --- Draw Lists: helper to read a (items, stream) pair (optional) ---
    auto readDrawPair = [&](const ChunkRecord* cList, const ChunkRecord* cIDs, Vector<DrawItem>& outItems, Vector<u32>& outIDs) {
        outItems.clear();
        outIDs.clear();
        if (!cList || !cIDs)
            return;

        // IDs
        IE_Assert((cIDs->size % sizeof(u32)) == 0);
        const u32 idCount = static_cast<u32>(cIDs->size / sizeof(u32));
        outIDs.resize(idCount);
        std::memcpy(outIDs.data(), blob + cIDs->offset, cIDs->size);

        // Items
        IE_Assert((cList->size % sizeof(DrawItem)) == 0);
        const u32 count = static_cast<u32>(cList->size / sizeof(DrawItem));
        outItems.resize(count);

        const auto* di = reinterpret_cast<const DrawItem*>(blob + cList->offset);
        if (count)
        {
            auto I = make_index(count);
            std::for_each(std::execution::par_unseq, I.begin(), I.end(), [&](u32 i) {
                DrawItem o{};
                o.primIndex = di[i].primIndex;
                IE_Assert(o.primIndex < m_index.size());
                o.firstIndex = di[i].firstIndex;
                o.indexCount = di[i].indexCount;
                o.instanceBegin = di[i].instanceBegin;
                o.instanceCount = di[i].instanceCount;
                o.materialIndex = di[i].materialIndex;
                o.sortKey = di[i].sortKey;

#ifndef NDEBUG
                IE_Assert(o.instanceBegin + o.instanceCount <= outIDs.size());
#endif
                outItems[i] = o;
            });
        }
    };

    // Culled buckets
    readDrawPair(cDRL[PIPELINE_OPAQUE], cDRI[PIPELINE_OPAQUE], m_drl[PIPELINE_OPAQUE], m_dri[PIPELINE_OPAQUE]);
    readDrawPair(cDRL[PIPELINE_MASKED], cDRI[PIPELINE_MASKED], m_drl[PIPELINE_MASKED], m_dri[PIPELINE_MASKED]);
    readDrawPair(cDRL[PIPELINE_BLENDED], cDRI[PIPELINE_BLENDED], m_drl[PIPELINE_BLENDED], m_dri[PIPELINE_BLENDED]);

    // No-cull buckets
    readDrawPair(cDNL[PIPELINE_OPAQUE], cDNI[PIPELINE_OPAQUE], m_dnl[PIPELINE_OPAQUE], m_dni[PIPELINE_OPAQUE]);
    readDrawPair(cDNL[PIPELINE_MASKED], cDNI[PIPELINE_MASKED], m_dnl[PIPELINE_MASKED], m_dni[PIPELINE_MASKED]);
    readDrawPair(cDNL[PIPELINE_BLENDED], cDNI[PIPELINE_BLENDED], m_dnl[PIPELINE_BLENDED], m_dni[PIPELINE_BLENDED]);
}

const PackedPrimitiveView* SceneLoader::GetPrimitiveById(u32 primId) const
{
    if (primId >= s_views.size())
        return nullptr;
    return &s_views[primId];
}

const u8* SceneLoader::GetTextureBlobData() const
{
    if (m_texBlobSize == 0)
        return nullptr;
    // Start of CH_TXTB
    const u8* base = static_cast<const u8*>(g_mapped.view);
    IE_Assert(base != nullptr);
    return base + m_texBlobOffset;
}

size_t SceneLoader::GetTextureBlobSize() const
{
    return static_cast<size_t>(m_texBlobSize);
}
