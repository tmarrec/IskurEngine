// Iskur Engine - Scene Packer
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <DirectXMath.h>

#include "common/Types.h"

namespace IEPack
{
inline constexpr char PACK_FILE_EXTENSION[] = ".ikp";

constexpr u32 PACK_VERSION_LATEST = 19;

constexpr u32 FourCC(char a, char b, char c, char d)
{
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) | (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

// Chunk IDs
enum : u32
{
    CH_PRIM = FourCC('P', 'R', 'I', 'M'),
    CH_VERT = FourCC('V', 'E', 'R', 'T'),
    CH_INDX = FourCC('I', 'N', 'D', 'X'),
    CH_MSHL = FourCC('M', 'S', 'H', 'L'),
    CH_MLVT = FourCC('M', 'L', 'V', 'T'),
    CH_MLTR = FourCC('M', 'L', 'T', 'R'),
    CH_MLBD = FourCC('M', 'L', 'B', 'D'),
    CH_OMIX = FourCC('O', 'M', 'I', 'X'),
    CH_OMDS = FourCC('O', 'M', 'D', 'S'),
    CH_OMDT = FourCC('O', 'M', 'D', 'T'),

    // Textures
    CH_TXHD = FourCC('T', 'X', 'H', 'D'),
    CH_TXSR = FourCC('T', 'X', 'S', 'R'),
    CH_TXTB = FourCC('T', 'X', 'T', 'B'),

    // Samplers / Materials / Instances
    CH_SAMP = FourCC('S', 'A', 'M', 'P'),
    CH_MATL = FourCC('M', 'A', 'T', 'L'),
    CH_INST = FourCC('I', 'N', 'S', 'T'),
};

// Material flags
enum : u32
{
    MATF_ALPHA_MASK = 1u << 0,
    MATF_ALPHA_BLEND = 1u << 1,
    MATF_DOUBLE_SIDED = 1u << 2,
};

#pragma pack(push, 1)

// Chunk table entry (no crc/flags)
struct ChunkRecord
{
    u32 id;
    u64 offset;
    u64 size;
};

struct PackHeader
{
    char magic[9];
    u32 version;
    u32 primCount;
    u32 chunkCount;
    u32 reserved0;
    u64 chunkTableOffset;
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
    u32 meshIndex, primIndex, materialIndex;
    u32 vertexCount, indexCount, meshletCount;
    u64 vertexByteOffset, indexByteOffset, meshletsByteOffset, mlVertsByteOffset, mlTrisByteOffset, mlBoundsByteOffset;
    u32 mlVertsCount, mlTrisByteCount;
    u32 ommIndexOffset, ommIndexCount;
    u32 ommDescOffset, ommDescCount;
    u64 ommDataByteOffset;
    u32 ommDataByteSize, ommFormat;

    // Primitive-local bounding sphere (object space).
    DirectX::XMFLOAT3 localBoundsCenter;
    f32 localBoundsRadius;
};
static_assert(sizeof(PrimRecord) == 128);

struct OpacityMicromapDescRecord
{
    u32 dataByteOffset;
    u32 dataByteSize;
    u32 subdivisionLevel;
    u32 reserved;
};
static_assert(sizeof(OpacityMicromapDescRecord) == 16);

// Texture table entry
struct TextureRecord
{
    u32 imageIndex;
    u32 format;
    u32 dimension;
    u32 miscFlags;
    u32 miscFlags2;
    u32 width;
    u32 height;
    u32 depth;
    u32 arraySize;
    u32 mipLevels;
    u32 subresourceOffset;
    u32 subresourceCount;
    u64 byteOffset, byteSize;
};

struct TextureSubresourceRecord
{
    u64 byteOffset;
    u64 byteSize;
    u32 rowPitch;
    u32 slicePitch;
};

// On-disk Material
struct MaterialRecord
{
    // Texture indices into TXHD (or -1 for none)
    i32 baseColorTx, normalTx, metallicRoughTx, occlusionTx, emissiveTx;
    // Sampler indices into SAMP (UINT32_MAX for none)
    u32 baseColorSampler, normalSampler, metallicRoughSampler, occlusionSampler, emissiveSampler;

    f32 baseColorFactor[4], emissiveFactor[3], metallicFactor, roughnessFactor, normalScale, occlusionStrength, alphaCutoff;
    u32 flags; // MATF_ALPHA_* | MATF_DOUBLE_SIDED
};

struct InstanceRecord
{
    u32 primIndex;             // global prim index into PRIM table
    u32 materialIndex;         // final resolved material for this instance
    DirectX::XMFLOAT4X4 world; // row-major 3x4
};

#pragma pack(pop)
} // namespace IEPack
