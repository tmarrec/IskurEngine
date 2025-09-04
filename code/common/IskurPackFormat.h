// Iškur Engine - Shared pack format
// MIT License
#pragma once

#include <cstdint>

namespace IEPack
{
// Short type aliases
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// Utility
constexpr u32 FourCC(char a, char b, char c, char d)
{
    return (u32(u8(a))) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

// Chunk IDs (v5)
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

    // Draw lists (culled)
    CH_DRL0 = FourCC('D', 'R', 'L', '0'),
    CH_DRI0 = FourCC('D', 'R', 'I', '0'),
    CH_DRL1 = FourCC('D', 'R', 'L', '1'),
    CH_DRI1 = FourCC('D', 'R', 'I', '1'),
    CH_DRL2 = FourCC('D', 'R', 'L', '2'),
    CH_DRI2 = FourCC('D', 'R', 'I', '2'),

    // Draw lists (no-cull)
    CH_DNL0 = FourCC('D', 'N', 'L', '0'),
    CH_DNI0 = FourCC('D', 'N', 'I', '0'),
    CH_DNL1 = FourCC('D', 'N', 'L', '1'),
    CH_DNI1 = FourCC('D', 'N', 'I', '1'),
    CH_DNL2 = FourCC('D', 'N', 'L', '2'),
    CH_DNI2 = FourCC('D', 'N', 'I', '2'),
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

// Keep D3D12 sampler encodings as-is (on-disk representation)
enum : u32
{
    D3D12_TAM_WRAP = 1,
    D3D12_TAM_MIRROR = 2,
    D3D12_TAM_CLAMP = 3,
    D3D12_TAM_BORDER = 4,
    D3D12_CF_NEVER = 1,
    D3D12_FILTER_MIN_MAG_MIP_POINT = 0x00,
    D3D12_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D12_FILTER_ANISOTROPIC = 0x55,
};

#pragma pack(push, 1)

// Chunk table entry (no crc/flags)
struct ChunkRecord
{
    u32 id;
    u64 offset;
    u64 size;
};

// File header (v5)
struct PackHeader
{
    char magic[8];
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
using PackHeaderV5 = PackHeader; // alias used by the loader

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

// Sampler table entry (D3D12 encoding)
struct SamplerDisk
{
    u32 d3d12Filter, addressU, addressV, addressW;
    float mipLODBias, minLOD, maxLOD;
    u32 maxAnisotropy, comparisonFunc;
    float borderColor[4];
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

// On-disk Instance
struct InstanceRecord
{
    u32 primIndex, materialOverride; // primIndex is GLOBAL packed-prim id
    XMFLOAT4X4 world;                // row-major 3x4
    float aabbMin[3], aabbMax[3];
    float bsCenter[3];
    float bsRadius;
    float maxScale; // spectral norm of linear part
};

// On-disk Draw item
struct DrawItem
{
    u32 primIndex, firstIndex, indexCount, instanceBegin, instanceCount, materialIndex;
    u64 sortKey;
    XMFLOAT4X4 world;
    u32 doubleSided;
    u32 alphaMode;
};

#pragma pack(pop)
} // namespace IEPack
