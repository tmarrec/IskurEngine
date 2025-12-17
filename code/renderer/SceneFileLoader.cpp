// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneFileLoader.h"

#include "common/Asserts.h"
#include "common/IskurPackFormat.h"
#include "shaders/CPUGPU.h"

#include <filesystem>
#include <fstream>

namespace
{
using namespace IEPack;

bool magic_ok(const char magic[9])
{
    constexpr char mk[9] = {'I', 'S', 'K', 'U', 'R', 'P', 'A', 'C', 'K'};
    return std::memcmp(magic, mk, 9) == 0;
}

Vector<u8> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    IE_Assert(f.is_open());

    const auto endPos = f.tellg();
    IE_Assert(endPos > 0);
    const size_t fileSize = endPos;

    Vector<u8> bytes(fileSize);
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    IE_Assert(f.good());

    return bytes;
}

void CopyChunkBytes(Vector<u8>& dst, const ChunkRecord* ch, const u8* blob, u64 blobSize)
{
    if (!ch)
    {
        dst.clear();
        return;
    }

    IE_Assert(ch->offset + ch->size <= blobSize);
    const size_t sz = ch->size;

    dst.resize(sz);
    if (sz)
        std::memcpy(dst.data(), blob + ch->offset, sz);
}

template <typename T> void CopyChunkArray(Vector<T>& dst, const ChunkRecord* ch, const u8* blob, u64 blobSize)
{
    if (!ch)
    {
        dst.clear();
        return;
    }

    IE_Assert(ch->size % sizeof(T) == 0);
    IE_Assert(ch->offset + ch->size <= blobSize);

    const u32 count = static_cast<u32>(ch->size / sizeof(T));
    const auto* src = reinterpret_cast<const T*>(blob + ch->offset);

    dst.resize(count);
    if (count)
        std::memcpy(dst.data(), src, static_cast<size_t>(count) * sizeof(T));
}

} // namespace

SceneFileData LoadSceneFile(const std::filesystem::path& packFile)
{
    Vector<u8> fileBytes = ReadFileBytes(packFile);

    const u8* const blob = fileBytes.data();
    const u64 blobSize = fileBytes.size();
    IE_Assert(blob && blobSize > 0);

    SceneFileData out{};

    const auto* hdr = reinterpret_cast<const PackHeader*>(blob);
    IE_Assert(magic_ok(hdr->magic));
    IE_Assert(hdr->version == 9);

    const auto* chunks = reinterpret_cast<const ChunkRecord*>(blob + hdr->chunkTableOffset);

    auto findChunk = [&](u32 id) -> const ChunkRecord* {
        for (u32 i = 0; i < hdr->chunkCount; ++i)
            if (chunks[i].id == id)
                return &chunks[i];
        return nullptr;
    };

    const auto* cPRIM = findChunk(CH_PRIM);
    const auto* cVERT = findChunk(CH_VERT);
    const auto* cINDX = findChunk(CH_INDX);
    const auto* cMSHL = findChunk(CH_MSHL);
    const auto* cMLVT = findChunk(CH_MLVT);
    const auto* cMLTR = findChunk(CH_MLTR);
    const auto* cMLBD = findChunk(CH_MLBD);
    const auto* cTXHD = findChunk(CH_TXHD);
    const auto* cTXTB = findChunk(CH_TXTB);
    const auto* cSAMP = findChunk(CH_SAMP);
    const auto* cMATL = findChunk(CH_MATL);
    const auto* cINST = findChunk(CH_INST);

    IE_Assert(cPRIM && cVERT && cINDX && cMSHL && cMLVT && cMLTR && cMLBD);

    // Geometry blobs
    CopyChunkBytes(out.vertBlob, cVERT, blob, blobSize);
    CopyChunkBytes(out.idxBlob, cINDX, blob, blobSize);
    CopyChunkBytes(out.mshlBlob, cMSHL, blob, blobSize);
    CopyChunkBytes(out.mlvtBlob, cMLVT, blob, blobSize);
    CopyChunkBytes(out.mltrBlob, cMLTR, blob, blobSize);
    CopyChunkBytes(out.mlbdBlob, cMLBD, blob, blobSize);

    // Primitives
    IE_Assert(cPRIM->size >= static_cast<u64>(hdr->primCount) * sizeof(PrimRecord));
    {
        const auto* prim = reinterpret_cast<const PrimRecord*>(blob + cPRIM->offset);
        out.prims.assign(prim, prim + hdr->primCount);
    }

    // Textures
    if (cTXHD && cTXTB)
    {
        IE_Assert(cTXHD->size % sizeof(TextureRecord) == 0);
        const u32 txCount = static_cast<u32>(cTXHD->size / sizeof(TextureRecord));

        IE_Assert(cTXTB->offset + cTXTB->size <= blobSize);

        out.texTable.resize(txCount);
        const auto* txDisk = reinterpret_cast<const TextureRecord*>(blob + cTXHD->offset);

        if (txCount)
        {
            for (u32 i = 0; i < txCount; ++i)
                IE_Assert(txDisk[i].byteOffset + txDisk[i].byteSize <= cTXTB->size);

            std::memcpy(out.texTable.data(), txDisk, static_cast<size_t>(txCount) * sizeof(TextureRecord));
        }

        const size_t txBlobSize = cTXTB->size;
        out.texBlob.resize(txBlobSize);
        if (txBlobSize)
            std::memcpy(out.texBlob.data(), blob + cTXTB->offset, txBlobSize);
    }

    // Samplers & materials (plain arrays)
    CopyChunkArray<D3D12_SAMPLER_DESC>(out.samplers, cSAMP, blob, blobSize);
    CopyChunkArray<MaterialRecord>(out.materials, cMATL, blob, blobSize);

    // Instances (grouped by primIndex)
    if (cINST)
    {
        IE_Assert(cINST->size % sizeof(InstanceRecord) == 0);
        IE_Assert(cINST->offset + cINST->size <= blobSize);

        const u32 instCount = static_cast<u32>(cINST->size / sizeof(InstanceRecord));
        const auto* id = reinterpret_cast<const InstanceRecord*>(blob + cINST->offset);

        const u32 primCount = static_cast<u32>(out.prims.size());
        IE_Assert(primCount > 0);

        Vector<u32> counts(primCount, 0);
        for (u32 i = 0; i < instCount; ++i)
        {
            const auto& it = id[i];
            IE_Assert(it.primIndex < primCount);
            counts[it.primIndex]++;
        }

        Vector<u32> offsets(primCount + 1, 0);
        u32 run = 0;
        for (u32 p = 0; p < primCount; ++p)
        {
            offsets[p] = run;
            run += counts[p];
        }
        offsets[primCount] = run;

        out.instances.resize(run);
        Vector<u32> cursor(offsets.begin(), offsets.end() - 1);

        for (u32 i = 0; i < instCount; ++i)
        {
            const auto& it = id[i];
            const u32 p = it.primIndex;
            out.instances[cursor[p]++] = it;
        }
    }

    return out;
}
