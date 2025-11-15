// Iškur Engine - Scene Packer
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <DirectXMath.h>
#include <cstdint>

namespace IEPack
{
// Short type aliases
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

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

    // Textures
    CH_TXHD = FourCC('T', 'X', 'H', 'D'),
    CH_TXTB = FourCC('T', 'X', 'T', 'B'),

    // Samplers / Materials / Instances
    CH_SAMP = FourCC('S', 'A', 'M', 'P'),
    CH_MATL = FourCC('M', 'A', 'T', 'L'),
    CH_INST = FourCC('I', 'N', 'S', 'T'),
};

// Texture/material flags
enum : u32
{
    TEXFLAG_SRGB = 1u << 0,
    TEXFLAG_NORMAL = 1u << 1,

    MATF_ALPHA_OPAQUE = 0,
    MATF_ALPHA_MASK = 1u << 0,
    MATF_ALPHA_BLEND = 1u << 1,
    MATF_DOUBLE_SIDED = 1u << 2,
    MATF_HAS_BC = 1u << 3,
    MATF_HAS_NORM = 1u << 4,
    MATF_HAS_MR = 1u << 5,
    MATF_HAS_OCC = 1u << 6,
    MATF_HAS_EMISSIVE = 1u << 7,
    MATF_UV_XFORM = 1u << 8,
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
};

// Texture table entry
struct TextureRecord
{
    u32 imageIndex, flags;
    u64 byteOffset, byteSize;
};

// On-disk Material
struct MaterialRecord
{
    // Texture indices into TXHD (or -1 for none)
    i32 baseColorTx, normalTx, metallicRoughTx, occlusionTx, emissiveTx;
    // Sampler indices into SAMP (UINT32_MAX for none)
    u32 baseColorSampler, normalSampler, metallicRoughSampler, occlusionSampler, emissiveSampler;

    float baseColorFactor[4], emissiveFactor[3], metallicFactor, roughnessFactor, normalScale, occlusionStrength, alphaCutoff;
    u32 flags; // MATF_*
    float uvScale[2], uvOffset[2], uvRotation;
    u32 _pad1;
};

struct InstanceRecord
{
    u32 primIndex;     // global prim index into PRIM table
    u32 materialIndex; // final resolved material for this instance
    XMFLOAT4X4 world;  // row-major 3x4
};

#pragma pack(pop)

static_assert(sizeof(InstanceRecord) == (sizeof(u32) * 2 + sizeof(XMFLOAT4X4)), "InstanceRecord size mismatch (v9)");

} // namespace IEPack
