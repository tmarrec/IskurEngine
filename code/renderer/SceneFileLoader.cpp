// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneFileLoader.h"

#include "common/IskurPackFormat.h"
#include "shaders/CPUGPU.h"

namespace
{
using namespace IEPack;

bool IsSubrangeValid(u64 offset, u64 size, u64 totalSize)
{
    return offset <= totalSize && size <= (totalSize - offset);
}

template <typename T> bool IsCountedRangeValid(u64 offset, u32 count, u64 totalSize)
{
    if (offset > totalSize)
        return false;
    const u64 remaining = totalSize - offset;
    return static_cast<u64>(count) <= (remaining / sizeof(T));
}

bool MagicOk(const char magic[9])
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

void SetChunkView(const ChunkRecord* ch, u64 blobSize, u64& outOffset, u64& outSize)
{
    outOffset = 0;
    outSize = 0;
    if (!ch)
        return;
    IE_Assert(IsSubrangeValid(ch->offset, ch->size, blobSize));
    outOffset = ch->offset;
    outSize = ch->size;
}

template <typename T> void CopyChunkArray(Vector<T>& dst, const ChunkRecord* ch, const u8* blob, u64 blobSize)
{
    if (!ch)
    {
        dst.clear();
        return;
    }

    IE_Assert(ch->size % sizeof(T) == 0);
    IE_Assert(IsSubrangeValid(ch->offset, ch->size, blobSize));

    const u32 count = static_cast<u32>(ch->size / sizeof(T));
    const auto* src = reinterpret_cast<const T*>(blob + ch->offset);

    dst.resize(count);
    if (count)
        std::memcpy(dst.data(), src, static_cast<size_t>(count) * sizeof(T));
}

} // namespace

SceneFileData LoadSceneFile(const std::filesystem::path& packFile)
{
    SceneFileData out{};
    out.fileBytes = ReadFileBytes(packFile);

    const u8* const blob = out.fileBytes.data();
    const u64 blobSize = out.fileBytes.size();
    IE_Assert(blob && blobSize > 0);
    IE_Assert(blobSize >= sizeof(PackHeader));

    const auto* hdr = reinterpret_cast<const PackHeader*>(blob);
    IE_Assert(MagicOk(hdr->magic));
    IE_Assert(hdr->version == PACK_VERSION_LATEST);
    IE_Assert(IsCountedRangeValid<ChunkRecord>(hdr->chunkTableOffset, hdr->chunkCount, blobSize));

    const auto* chunks = reinterpret_cast<const ChunkRecord*>(blob + hdr->chunkTableOffset);
    for (u32 i = 0; i < hdr->chunkCount; ++i)
    {
        IE_Assert(IsSubrangeValid(chunks[i].offset, chunks[i].size, blobSize));
    }

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
    const auto* cTXSR = findChunk(CH_TXSR);
    const auto* cTXTB = findChunk(CH_TXTB);
    const auto* cSAMP = findChunk(CH_SAMP);
    const auto* cMATL = findChunk(CH_MATL);
    const auto* cINST = findChunk(CH_INST);

    IE_Assert(cPRIM && cVERT && cINDX && cMSHL && cMLVT && cMLTR && cMLBD);

    SetChunkView(cVERT, blobSize, out.vertBlobOffset, out.vertBlobSize);
    SetChunkView(cINDX, blobSize, out.idxBlobOffset, out.idxBlobSize);
    SetChunkView(cMSHL, blobSize, out.mshlBlobOffset, out.mshlBlobSize);
    SetChunkView(cMLVT, blobSize, out.mlvtBlobOffset, out.mlvtBlobSize);
    SetChunkView(cMLTR, blobSize, out.mltrBlobOffset, out.mltrBlobSize);
    SetChunkView(cMLBD, blobSize, out.mlbdBlobOffset, out.mlbdBlobSize);

    IE_Assert(hdr->primCount <= (cPRIM->size / sizeof(PrimRecord)));
    {
        const auto* prim = reinterpret_cast<const PrimRecord*>(blob + cPRIM->offset);
        out.prims.assign(prim, prim + hdr->primCount);
    }

    if (cTXHD && cTXTB)
    {
        IE_Assert(cTXSR);
        IE_Assert(cTXHD->size % sizeof(TextureRecord) == 0);
        const u32 txCount = static_cast<u32>(cTXHD->size / sizeof(TextureRecord));

        IE_Assert(cTXSR->size % sizeof(TextureSubresourceRecord) == 0);
        const u32 txSubresourceCount = static_cast<u32>(cTXSR->size / sizeof(TextureSubresourceRecord));

        IE_Assert(IsSubrangeValid(cTXTB->offset, cTXTB->size, blobSize));

        out.texTable.resize(txCount);
        const auto* txDisk = reinterpret_cast<const TextureRecord*>(blob + cTXHD->offset);
        const auto* txSubresourceDisk = reinterpret_cast<const TextureSubresourceRecord*>(blob + cTXSR->offset);
        out.texSubresources.assign(txSubresourceDisk, txSubresourceDisk + txSubresourceCount);

        if (txCount)
        {
            for (u32 i = 0; i < txCount; ++i)
            {
                IE_Assert(txDisk[i].byteOffset + txDisk[i].byteSize <= cTXTB->size);
                IE_Assert(txDisk[i].subresourceOffset <= txSubresourceCount);
                IE_Assert(txDisk[i].subresourceCount <= (txSubresourceCount - txDisk[i].subresourceOffset));
            }

            std::memcpy(out.texTable.data(), txDisk, static_cast<size_t>(txCount) * sizeof(TextureRecord));
        }

        SetChunkView(cTXTB, blobSize, out.texBlobOffset, out.texBlobSize);
    }

    CopyChunkArray<D3D12_SAMPLER_DESC>(out.samplers, cSAMP, blob, blobSize);
    CopyChunkArray<MaterialRecord>(out.materials, cMATL, blob, blobSize);

    if (cINST)
    {
        IE_Assert(cINST->size % sizeof(InstanceRecord) == 0);
        IE_Assert(cINST->offset + cINST->size <= blobSize);

        const u32 instCount = static_cast<u32>(cINST->size / sizeof(InstanceRecord));
        const auto* id = reinterpret_cast<const InstanceRecord*>(blob + cINST->offset);
        const u32 primCount = static_cast<u32>(out.prims.size());
        IE_Assert(primCount > 0);

        out.instances.assign(id, id + instCount);
    }

    return out;
}
