// Iškur Engine - Scene Packer
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "common/IskurPackFormat.h"
#include <DirectXMath.h>
#include <DirectXTex.h>
#include <Objbase.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <filesystem>
#include <fstream>
#include <meshoptimizer.h>
#include <mikktspace.h>
#include <print>
#include <string>
#include <tiny_gltf.h>
#include <unordered_map>
#include <vector>

using namespace DirectX;
namespace fs = std::filesystem;

struct IskurMeshlet
{
    uint32_t vertexOffset, triangleOffset;
    uint16_t vertexCount, triangleCount;
};

using namespace IEPack;

static void TRS_ToColArray(const float T[3], const float Qraw[4], const float S[3], float M[16])
{
    float x = Qraw[0], y = Qraw[1], z = Qraw[2], w = Qraw[3];
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0)
    {
        x /= n;
        y /= n;
        z /= n;
        w /= n;
    }
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    M[0] = (1.f - 2.f * (yy + zz)) * S[0];
    M[1] = (2.f * (xy + wz)) * S[0];
    M[2] = (2.f * (xz - wy)) * S[0];
    M[3] = 0;
    M[4] = (2.f * (xy - wz)) * S[1];
    M[5] = (1.f - 2.f * (xx + zz)) * S[1];
    M[6] = (2.f * (yz + wx)) * S[1];
    M[7] = 0;
    M[8] = (2.f * (xz + wy)) * S[2];
    M[9] = (2.f * (yz - wx)) * S[2];
    M[10] = (1.f - 2.f * (xx + yy)) * S[2];
    M[11] = 0;
    M[12] = T[0];
    M[13] = T[1];
    M[14] = T[2];
    M[15] = 1;
}

static XMMATRIX RowMatFromColArray(const float Mc[16])
{
    return XMMatrixSet(Mc[0], Mc[1], Mc[2], Mc[3], Mc[4], Mc[5], Mc[6], Mc[7], Mc[8], Mc[9], Mc[10], Mc[11], Mc[12], Mc[13], Mc[14], Mc[15]);
}

static XMMATRIX NodeLocalMatrix_Row(const tinygltf::Node& n)
{
    if (n.matrix.size() == 16)
    {
        float Mc[16];
        for (int i = 0; i < 16; ++i)
            Mc[i] = static_cast<float>(n.matrix[i]);
        return RowMatFromColArray(Mc);
    }

    float T[3]{0, 0, 0}, S[3]{1, 1, 1}, Q[4]{0, 0, 0, 1};
    if (n.translation.size() == 3)
    {
        T[0] = static_cast<float>(n.translation[0]);
        T[1] = static_cast<float>(n.translation[1]);
        T[2] = static_cast<float>(n.translation[2]);
    }
    if (n.scale.size() == 3)
    {
        S[0] = static_cast<float>(n.scale[0]);
        S[1] = static_cast<float>(n.scale[1]);
        S[2] = static_cast<float>(n.scale[2]);
    }
    if (n.rotation.size() == 4)
    {
        Q[0] = static_cast<float>(n.rotation[0]);
        Q[1] = static_cast<float>(n.rotation[1]);
        Q[2] = static_cast<float>(n.rotation[2]);
        Q[3] = static_cast<float>(n.rotation[3]);
    }

    float Mc[16];
    TRS_ToColArray(T, Q, S, Mc);
    return RowMatFromColArray(Mc);
}

static bool IsGlbFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    char m[4]{};
    f.read(m, 4);
    return f && m[0] == 'g' && m[1] == 'l' && m[2] == 'T' && m[3] == 'F';
}

static HRESULT LoadAnyImageMemory(const uint8_t* bytes, size_t size, ScratchImage& out)
{
    if (size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
        return LoadFromDDSMemory(bytes, size, DDS_FLAGS_NONE, nullptr, out);
    if (size >= 10 && std::memcmp(bytes, "#?RADIANCE", 10) == 0)
        return LoadFromHDRMemory(bytes, size, nullptr, out);
    return LoadFromWICMemory(bytes, size, WIC_FLAGS_NONE, nullptr, out);
}

enum : uint32_t
{
    IMG_BASECOLOR = 1u << 0,
    IMG_NORMAL = 1u << 1,
    IMG_METALROUGH = 1u << 2,
    IMG_OCCLUSION = 1u << 3,
    IMG_EMISSIVE = 1u << 4
};

static std::vector<uint32_t> BuildImageUsageFlags(const tinygltf::Model& model)
{
    std::vector<uint32_t> flags(model.images.size(), 0);
    auto mark = [&](int ti, uint32_t f) {
        if (ti < 0 || ti >= static_cast<int>(model.textures.size()))
            return;
        int img = model.textures[static_cast<size_t>(ti)].source;
        if (img < 0 || img >= static_cast<int>(flags.size()))
            return;
        flags[static_cast<size_t>(img)] |= f;
    };
    for (const auto& m : model.materials)
    {
        mark(m.pbrMetallicRoughness.baseColorTexture.index, IMG_BASECOLOR);
        mark(m.normalTexture.index, IMG_NORMAL);
        mark(m.pbrMetallicRoughness.metallicRoughnessTexture.index, IMG_METALROUGH);
        mark(m.occlusionTexture.index, IMG_OCCLUSION);
        mark(m.emissiveTexture.index, IMG_EMISSIVE);
    }
    return flags;
}

static uint32_t AccessorStrideBytes(const tinygltf::Accessor& a, const tinygltf::BufferView& v)
{
    uint32_t s = a.ByteStride(v);
    if (s)
        return s;
    return static_cast<uint32_t>(tinygltf::GetNumComponentsInType(a.type) * tinygltf::GetComponentSizeInBytes(a.componentType));
}

// ---- MikkTSpace ----
static int mk_getNumFaces(const SMikkTSpaceContext* ctx);
static int mk_getNumVerticesOfFace(const SMikkTSpaceContext*, const int)
{
    return 3;
}
static void mk_getPosition(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v);
static void mk_getNormal(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v);
static void mk_getTexCoord(const SMikkTSpaceContext* ctx, float out[2], const int f, const int v);
static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const float t[3], const float sign, const int f, const int v);

struct MikkUserData
{
    const std::vector<uint32_t>* indices;
    std::vector<Vertex>* verts;
};

static void ComputeTangentsMikk(const std::vector<uint32_t>& idx, std::vector<Vertex>& verts)
{
    SMikkTSpaceInterface i{};
    i.m_getNumFaces = mk_getNumFaces;
    i.m_getNumVerticesOfFace = mk_getNumVerticesOfFace;
    i.m_getPosition = mk_getPosition;
    i.m_getNormal = mk_getNormal;
    i.m_getTexCoord = mk_getTexCoord;
    i.m_setTSpaceBasic = mk_setTSpaceBasic;

    MikkUserData ud{&idx, &verts};
    SMikkTSpaceContext c{};
    c.m_pInterface = &i;
    c.m_pUserData = &ud;
    genTangSpaceDefault(&c);
}

static int mk_getNumFaces(const SMikkTSpaceContext* ctx)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    return static_cast<int>(ud->indices->size() / 3);
}

static void mk_getPosition(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    auto& P = (*ud->verts)[i].position;
    out[0] = P.x;
    out[1] = P.y;
    out[2] = P.z;
}

static void mk_getNormal(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    auto& N = (*ud->verts)[i].normal;
    out[0] = N.x;
    out[1] = N.y;
    out[2] = N.z;
}

static void mk_getTexCoord(const SMikkTSpaceContext* ctx, float out[2], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    auto& uv = (*ud->verts)[i].texCoord;
    out[0] = uv.x;
    out[1] = uv.y;
}

static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const float t[3], const float s, const int f, const int v)
{
    auto* ud = static_cast<MikkUserData*>(ctx->m_pUserData);
    uint32_t i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    auto& d = (*ud->verts)[i].tangent;
    d.x = t[0];
    d.y = t[1];
    d.z = t[2];
    d.w = -s;
}

static void BuildTexturesToMemory(const fs::path& glbPath, const tinygltf::Model& model, const std::vector<uint32_t>& imgUsage, bool fastCompress, std::vector<TextureRecord>& outTable,
                                  std::vector<uint8_t>& outBlob)
{
    std::vector<uint8_t> done(model.images.size(), 0);
    const fs::path baseDir = glbPath.parent_path();

    auto loadImageBytes = [&](size_t imgIndex, const uint8_t*& bytes, size_t& size, std::vector<uint8_t>& owned) -> bool {
        const auto& img = model.images[imgIndex];
        if (img.bufferView >= 0)
        {
            const auto& bv = model.bufferViews[static_cast<size_t>(img.bufferView)];
            const auto& buf = model.buffers[static_cast<size_t>(bv.buffer)];
            if (static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(bv.byteLength) > buf.data.size())
                return false;
            bytes = buf.data.data() + static_cast<size_t>(bv.byteOffset);
            size = static_cast<size_t>(bv.byteLength);
            return true;
        }
        if (!img.image.empty())
        {
            bytes = img.image.data();
            size = img.image.size();
            return true;
        }
        if (!img.uri.empty())
        {
            fs::path p = baseDir / fs::u8path(img.uri);
            std::ifstream f(p, std::ios::binary);
            if (!f.good())
                return false;
            f.seekg(0, std::ios::end);
            size_t sz = static_cast<size_t>(f.tellg());
            f.seekg(0, std::ios::beg);
            owned.resize(sz);
            f.read(reinterpret_cast<char*>(owned.data()), static_cast<std::streamsize>(sz));
            if (!f.good())
                return false;
            bytes = owned.data();
            size = sz;
            return true;
        }
        return false;
    };

    for (size_t t = 0; t < model.textures.size(); ++t)
    {
        int imgIdx = model.textures[t].source;
        if (imgIdx < 0 || imgIdx >= static_cast<int>(model.images.size()))
            continue;

        size_t i = static_cast<size_t>(imgIdx);
        if (done[i])
            continue;

        const auto& img = model.images[i];
        std::string srcDesc;
        if (!img.uri.empty())
            srcDesc = img.uri;
        else if (img.bufferView >= 0)
            srcDesc = std::string("bufferView#") + std::to_string(img.bufferView);
        else if (!img.image.empty())
            srcDesc = "embedded";
        else
            srcDesc = "unknown";

        size_t refCount = 0;
        for (size_t ti = 0; ti < model.textures.size(); ++ti)
            if (model.textures[ti].source == static_cast<int>(i))
                ++refCount;

        const uint8_t* raw = nullptr;
        size_t rawSize = 0;
        std::vector<uint8_t> owned;

        bool okLoad = loadImageBytes(i, raw, rawSize, owned);
        assert(okLoad && "Failed to load image bytes from glTF");

        std::println("[tex {}][img {}] refs={} src=\"{}\"", t, i, refCount, srcDesc);

        ScratchImage loaded;
        HRESULT hrDec = LoadAnyImageMemory(raw, rawSize, loaded);
        assert(SUCCEEDED(hrDec) && "Image decode failed");

        bool isNormal = (i < imgUsage.size()) && (imgUsage[i] & IMG_NORMAL);
        bool isSRGB = !isNormal && ((i < imgUsage.size()) && ((imgUsage[i] & (IMG_BASECOLOR | IMG_EMISSIVE)) && !(imgUsage[i] & (IMG_METALROUGH | IMG_OCCLUSION))));
        DXGI_FORMAT wantBase = isSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

        const Image* srcImages = nullptr;
        size_t srcCount = 0;
        TexMetadata meta{};
        ScratchImage converted;

        bool didConvert = loaded.GetMetadata().format != wantBase;
        if (didConvert)
        {
            HRESULT hrC = Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(), wantBase, TEX_FILTER_DEFAULT, 0.0f, converted);
            assert(SUCCEEDED(hrC) && "Image format conversion failed");
            srcImages = converted.GetImages();
            srcCount = converted.GetImageCount();
            meta = converted.GetMetadata();
        }
        else
        {
            srcImages = loaded.GetImages();
            srcCount = loaded.GetImageCount();
            meta = loaded.GetMetadata();
        }

        ScratchImage mip;
        if (SUCCEEDED(GenerateMipMaps(srcImages, srcCount, meta, TEX_FILTER_DEFAULT, 0, mip)))
        {
            srcImages = mip.GetImages();
            srcCount = mip.GetImageCount();
            meta = mip.GetMetadata();
        }

        DXGI_FORMAT compFmt = isNormal ? DXGI_FORMAT_BC5_UNORM : (isSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM);
        TEX_COMPRESS_FLAGS comp = TEX_COMPRESS_PARALLEL;
        if (!isNormal && fastCompress)
            comp |= TEX_COMPRESS_BC7_QUICK;

        ScratchImage bc;
        HRESULT hrComp = Compress(srcImages, srcCount, meta, compFmt, comp, 0.5f, bc);
        assert(SUCCEEDED(hrComp) && "BC compression failed");

        Blob blob;
        HRESULT hrSave = SaveToDDSMemory(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_FORCE_DX10_EXT, blob);
        assert(SUCCEEDED(hrSave) && "Saving DDS to memory failed");

        uint64_t off = outBlob.size();
        uint64_t sz = blob.GetBufferSize();
        outBlob.insert(outBlob.end(), reinterpret_cast<const uint8_t*>(blob.GetBufferPointer()), reinterpret_cast<const uint8_t*>(blob.GetBufferPointer()) + sz);

        TextureRecord tr;
        tr.imageIndex = static_cast<uint32_t>(i);
        tr.flags = (isSRGB ? TEXFLAG_SRGB : 0) | (isNormal ? TEXFLAG_NORMAL : 0);
        tr.byteOffset = off;
        tr.byteSize = sz;
        outTable.push_back(tr);
        done[i] = 1;
    }
}

static D3D12_TEXTURE_ADDRESS_MODE MapWrapModeGLTFToD3D12Addr(int wrap)
{
    switch (wrap)
    {
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

static D3D12_FILTER BakeD3D12Filter(int minFilter, int magFilter)
{
    if (minFilter <= 0)
        minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    if (magFilter <= 0)
        magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;

    if (minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    if (minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST || minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR || magFilter == TINYGLTF_TEXTURE_FILTER_LINEAR)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    return D3D12_FILTER_MIN_MAG_MIP_POINT;
}

static void BuildSamplerTable(const tinygltf::Model& model, std::vector<D3D12_SAMPLER_DESC>& outSamp)
{
    for (const tinygltf::Sampler& s : model.samplers)
    {
        D3D12_SAMPLER_DESC samplerDesc;
        samplerDesc.Filter = BakeD3D12Filter(s.minFilter, s.magFilter);
        samplerDesc.AddressU = MapWrapModeGLTFToD3D12Addr(s.wrapS);
        samplerDesc.AddressV = MapWrapModeGLTFToD3D12Addr(s.wrapT);
        samplerDesc.AddressW = samplerDesc.AddressV;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor[0] = 0.0f;
        samplerDesc.BorderColor[1] = 0.0f;
        samplerDesc.BorderColor[2] = 0.0f;
        samplerDesc.BorderColor[3] = 0.0f;
        outSamp.push_back(samplerDesc);
    }
}

static void BuildMaterialTable(const tinygltf::Model& model, const std::vector<TextureRecord>& texTable, std::vector<MaterialRecord>& outMatl)
{
    std::unordered_map<int, int> imageToTxhd;
    imageToTxhd.reserve(texTable.size());
    for (size_t i = 0; i < texTable.size(); ++i)
        imageToTxhd[static_cast<int>(texTable[i].imageIndex)] = static_cast<int>(i);

    auto texIndexToTxhd = [&](int texIndex) -> int {
        if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size()))
            return -1;
        int img = model.textures[static_cast<size_t>(texIndex)].source;
        auto it = imageToTxhd.find(img);
        return (img < 0 || it == imageToTxhd.end()) ? -1 : it->second;
    };

    auto parseAlphaMode = [](const tinygltf::Material& m) -> uint32_t {
        if (m.alphaMode == "MASK")
            return MATF_ALPHA_MASK;
        if (m.alphaMode == "BLEND")
            return MATF_ALPHA_BLEND;
        return 0;
    };

    const size_t matCount = std::max<size_t>(1, model.materials.size());
    outMatl.assign(matCount, MaterialRecord{});

    for (size_t i = 0; i < matCount; ++i)
    {
        MaterialRecord mr{};
        mr.baseColorTx = mr.normalTx = mr.metallicRoughTx = mr.occlusionTx = mr.emissiveTx = -1;
        mr.baseColorSampler = mr.normalSampler = mr.metallicRoughSampler = mr.occlusionSampler = mr.emissiveSampler = UINT32_MAX;
        mr.baseColorFactor[0] = mr.baseColorFactor[1] = mr.baseColorFactor[2] = mr.baseColorFactor[3] = 1.0f;
        mr.emissiveFactor[0] = mr.emissiveFactor[1] = mr.emissiveFactor[2] = 0.0f;
        mr.metallicFactor = 1.0f;
        mr.roughnessFactor = 1.0f;
        mr.normalScale = 1.0f;
        mr.occlusionStrength = 1.0f;
        mr.alphaCutoff = 0.5f;
        mr.uvScale[0] = mr.uvScale[1] = 1.0f;
        mr.uvOffset[0] = mr.uvOffset[1] = 0.0f;
        mr.uvRotation = 0.0f;
        mr.flags = 0;
        mr._pad1 = 0;

        if (!model.materials.empty())
        {
            const tinygltf::Material& m = model.materials[i];

            if (m.pbrMetallicRoughness.baseColorFactor.size() == 4)
                for (int k = 0; k < 4; ++k)
                    mr.baseColorFactor[k] = static_cast<float>(m.pbrMetallicRoughness.baseColorFactor[k]);

            mr.metallicFactor = static_cast<float>(m.pbrMetallicRoughness.metallicFactor);
            mr.roughnessFactor = static_cast<float>(m.pbrMetallicRoughness.roughnessFactor);

            if (m.emissiveFactor.size() == 3)
            {
                mr.emissiveFactor[0] = static_cast<float>(m.emissiveFactor[0]);
                mr.emissiveFactor[1] = static_cast<float>(m.emissiveFactor[1]);
                mr.emissiveFactor[2] = static_cast<float>(m.emissiveFactor[2]);
            }

            if (m.alphaMode == "MASK")
                mr.alphaCutoff = static_cast<float>(m.alphaCutoff);

            mr.flags |= parseAlphaMode(m);
            if (m.doubleSided)
                mr.flags |= MATF_DOUBLE_SIDED;

            auto chooseSampler = [&](int texIndex) -> uint32_t {
                if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size()))
                    return 0u;
                int s = model.textures[static_cast<size_t>(texIndex)].sampler;
                return (s < 0) ? 0u : static_cast<uint32_t>(s);
            };

            if (m.pbrMetallicRoughness.baseColorTexture.index >= 0)
            {
                const int ti = m.pbrMetallicRoughness.baseColorTexture.index;
                mr.baseColorTx = texIndexToTxhd(ti);
                if (mr.baseColorTx >= 0)
                {
                    mr.flags |= MATF_HAS_BC;
                    mr.baseColorSampler = chooseSampler(ti);
                }
            }

            if (m.normalTexture.index >= 0)
            {
                const int ti = m.normalTexture.index;
                mr.normalTx = texIndexToTxhd(ti);
                mr.normalScale = static_cast<float>(m.normalTexture.scale);
                if (mr.normalTx >= 0)
                {
                    mr.flags |= MATF_HAS_NORM;
                    mr.normalSampler = chooseSampler(ti);
                }
            }

            if (m.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
            {
                const int ti = m.pbrMetallicRoughness.metallicRoughnessTexture.index;
                mr.metallicRoughTx = texIndexToTxhd(ti);
                if (mr.metallicRoughTx >= 0)
                {
                    mr.flags |= MATF_HAS_MR;
                    mr.metallicRoughSampler = chooseSampler(ti);
                }
            }

            if (m.occlusionTexture.index >= 0)
            {
                const int ti = m.occlusionTexture.index;
                mr.occlusionTx = texIndexToTxhd(ti);
                mr.occlusionStrength = static_cast<float>(m.occlusionTexture.strength);
                if (mr.occlusionTx >= 0)
                {
                    mr.flags |= MATF_HAS_OCC;
                    mr.occlusionSampler = chooseSampler(ti);
                }
            }

            if (m.emissiveTexture.index >= 0)
            {
                const int ti = m.emissiveTexture.index;
                mr.emissiveTx = texIndexToTxhd(ti);
                if (mr.emissiveTx >= 0)
                {
                    mr.flags |= MATF_HAS_EMISSIVE;
                    mr.emissiveSampler = chooseSampler(ti);
                }
            }
        }

        outMatl[i] = mr;
    }
}

static PrimRecord BuildOnePrimitive(const tinygltf::Model& model, int meshIdx, int primIdx, std::vector<Vertex>& blobVertices, std::vector<uint32_t>& blobIndices,
                                    std::vector<IskurMeshlet>& blobMeshlets, std::vector<uint32_t>& blobMLVerts, std::vector<uint8_t>& blobMLTris, std::vector<MeshletBounds>& blobMLBounds)
{
    const auto& gltfPrim = model.meshes[static_cast<size_t>(meshIdx)].primitives[static_cast<size_t>(primIdx)];
    assert(gltfPrim.mode == TINYGLTF_MODE_TRIANGLES && "Only triangle primitives are supported");

    const uint32_t materialIndex = (gltfPrim.material >= 0) ? static_cast<uint32_t>(gltfPrim.material) : 0u;

    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;

    if (gltfPrim.attributes.contains("NORMAL"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("NORMAL"));
        const auto& view = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[view.buffer];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = acc.count, stride = AccessorStrideBytes(acc, view);
        normals.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(&normals[i], data + i * stride, sizeof(XMFLOAT3));
    }
    if (gltfPrim.attributes.contains("TEXCOORD_0"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("TEXCOORD_0"));
        const auto& view = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[view.buffer];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = acc.count, stride = AccessorStrideBytes(acc, view);
        texcoords.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(&texcoords[i], data + i * stride, sizeof(XMFLOAT2));
    }

    std::vector<Vertex> initialVertices;
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("POSITION"));
        const auto& view = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[view.buffer];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = acc.count, stride = AccessorStrideBytes(acc, view);
        initialVertices.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Vertex v{};
            std::memcpy(&v.position, data + i * stride, sizeof(XMFLOAT3));
            if (!normals.empty())
            {
                const XMFLOAT3 n = normals[i];
                float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                v.normal = (len > 0) ? XMFLOAT3(n.x / len, n.y / len, n.z / len) : XMFLOAT3(0, 0, 1);
            }
            else
            {
                v.normal = XMFLOAT3(0, 0, 0);
            }
            v.texCoord = (!texcoords.empty()) ? texcoords[i] : XMFLOAT2(0, 0);
            v.tangent = XMFLOAT4(0, 0, 0, 0);
            initialVertices[i] = v;
        }
    }

    std::vector<uint32_t> initialIndices;
    if (gltfPrim.indices >= 0)
    {
        const auto& acc = model.accessors[gltfPrim.indices];
        const auto& view = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[view.buffer];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = acc.count;
        initialIndices.resize(count);
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
        {
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = data[i];
        }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const auto* p = reinterpret_cast<const uint16_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = p[i];
        }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const auto* p = reinterpret_cast<const uint32_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = p[i];
        }
        else
        {
            assert(false && "Unsupported index component type");
        }
    }
    else
    {
        initialIndices.resize(initialVertices.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(initialVertices.size()); ++i)
            initialIndices[i] = i;
    }

    if (normals.empty())
    {
        for (size_t i = 0; i < initialIndices.size(); i += 3)
        {
            uint32_t i0 = initialIndices[i + 0], i1 = initialIndices[i + 1], i2 = initialIndices[i + 2];
            const XMFLOAT3 &v0 = initialVertices[i0].position, &v1 = initialVertices[i1].position, &v2 = initialVertices[i2].position;
            float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
            float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0)
            {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            XMFLOAT3 n(nx, ny, nz);
            initialVertices[i0].normal = n;
            initialVertices[i1].normal = n;
            initialVertices[i2].normal = n;
        }
    }
    if (!texcoords.empty())
        ComputeTangentsMikk(initialIndices, initialVertices);

    std::vector<uint32_t> remap(initialIndices.size());
    uint32_t totalVertices = meshopt_generateVertexRemap(remap.data(), initialIndices.data(), initialIndices.size(), initialVertices.data(), initialVertices.size(), sizeof(Vertex));
    std::vector<uint32_t> outIndices(initialIndices.size());
    meshopt_remapIndexBuffer(outIndices.data(), initialIndices.data(), initialIndices.size(), remap.data());
    std::vector<Vertex> outVertices(totalVertices);
    meshopt_remapVertexBuffer(outVertices.data(), initialVertices.data(), initialVertices.size(), sizeof(Vertex), remap.data());
    meshopt_optimizeVertexCache(outIndices.data(), outIndices.data(), outIndices.size(), outVertices.size());
    meshopt_optimizeOverdraw(outIndices.data(), outIndices.data(), outIndices.size(), &outVertices[0].position.x, outVertices.size(), sizeof(Vertex), 1.05f);
    meshopt_optimizeVertexFetch(outVertices.data(), outIndices.data(), outIndices.size(), outVertices.data(), outVertices.size(), sizeof(Vertex));

    constexpr uint64_t maxVertices = 64, maxTriangles = 124;
    constexpr float coneWeight = 0.f;
    const uint32_t maxMeshlets = meshopt_buildMeshletsBound(outIndices.size(), maxVertices, maxTriangles);
    std::vector<meshopt_Meshlet> temp(maxMeshlets);
    std::vector<uint32_t> mlVerts(maxMeshlets * maxVertices);
    std::vector<uint8_t> mlTris(maxMeshlets * maxTriangles * 3);
    uint32_t mc = meshopt_buildMeshlets(temp.data(), mlVerts.data(), mlTris.data(), outIndices.data(), outIndices.size(), &outVertices[0].position.x, outVertices.size(), sizeof(Vertex), maxVertices,
                                        maxTriangles, coneWeight);
    temp.resize(mc);
    if (!temp.empty())
    {
        const auto& last = temp.back();
        mlVerts.resize(last.vertex_offset + last.vertex_count);
        mlTris.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));
    }

    std::vector<MeshletBounds> mlBounds;
    mlBounds.reserve(temp.size());
    for (const auto& m : temp)
    {
        meshopt_optimizeMeshlet(&mlVerts[m.vertex_offset], &mlTris[m.triangle_offset], m.triangle_count, m.vertex_count);
        meshopt_Bounds b = meshopt_computeMeshletBounds(&mlVerts[m.vertex_offset], &mlTris[m.triangle_offset], m.triangle_count, &outVertices[0].position.x, outVertices.size(), sizeof(Vertex));
        MeshletBounds mb;
        mb.center = XMFLOAT3(b.center[0], b.center[1], b.center[2]);
        mb.radius = b.radius;
        mb.cone_apex = XMFLOAT3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
        mb.cone_axis = XMFLOAT3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
        mb.cone_cutoff = b.cone_cutoff;
        mb.coneAxisAndCutoff = (static_cast<uint32_t>(static_cast<uint8_t>(b.cone_axis_s8[0]))) | (static_cast<uint32_t>(static_cast<uint8_t>(b.cone_axis_s8[1])) << 8) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(b.cone_axis_s8[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b.cone_cutoff_s8)) << 24);
        mlBounds.push_back(mb);
    }

    std::vector<IskurMeshlet> meshlets(temp.size());
    for (size_t i = 0; i < temp.size(); ++i)
    {
        meshlets[i].vertexOffset = temp[i].vertex_offset;
        meshlets[i].triangleOffset = temp[i].triangle_offset;
        meshlets[i].vertexCount = temp[i].vertex_count;
        meshlets[i].triangleCount = temp[i].triangle_count;
    }

    PrimRecord r;
    r.meshIndex = static_cast<uint32_t>(meshIdx);
    r.primIndex = static_cast<uint32_t>(primIdx);
    r.materialIndex = materialIndex;
    r.vertexCount = static_cast<uint32_t>(outVertices.size());
    r.indexCount = static_cast<uint32_t>(outIndices.size());
    r.meshletCount = static_cast<uint32_t>(meshlets.size());
    r.mlVertsCount = static_cast<uint32_t>(mlVerts.size());
    r.mlTrisByteCount = static_cast<uint32_t>(mlTris.size());
    r.vertexByteOffset = blobVertices.size() * sizeof(Vertex);
    r.indexByteOffset = blobIndices.size() * sizeof(uint32_t);
    r.meshletsByteOffset = blobMeshlets.size() * sizeof(IskurMeshlet);
    r.mlVertsByteOffset = blobMLVerts.size() * sizeof(uint32_t);
    r.mlTrisByteOffset = blobMLTris.size();
    r.mlBoundsByteOffset = blobMLBounds.size() * sizeof(MeshletBounds);

    blobVertices.insert(blobVertices.end(), outVertices.begin(), outVertices.end());
    blobIndices.insert(blobIndices.end(), outIndices.begin(), outIndices.end());
    blobMeshlets.insert(blobMeshlets.end(), meshlets.begin(), meshlets.end());
    blobMLVerts.insert(blobMLVerts.end(), mlVerts.begin(), mlVerts.end());
    blobMLTris.insert(blobMLTris.end(), mlTris.begin(), mlTris.end());
    blobMLBounds.insert(blobMLBounds.end(), mlBounds.begin(), mlBounds.end());

    std::println("[prim] mesh={} prim={}  v={} i={} m={}", r.meshIndex, r.primIndex, r.vertexCount, r.indexCount, r.meshletCount);

    return r;
}

static void BuildMeshPrimToPrimIndex(const std::vector<PrimRecord>& prims, std::vector<std::vector<uint32_t>>& map, size_t meshCount)
{
    map.resize(meshCount);
    for (uint32_t pi = 0; pi < prims.size(); ++pi)
    {
        const auto& pr = prims[pi];
        auto& vec = map[pr.meshIndex];
        if (vec.size() <= pr.primIndex)
            vec.resize(pr.primIndex + 1, UINT32_MAX);
        vec[pr.primIndex] = pi;
    }
}

static void GatherInstances_Recursive(const tinygltf::Model& model, int nodeIndex, const std::vector<std::vector<uint32_t>>& meshPrimToPrimIndex, const XMMATRIX& parentWorld,
                                      std::vector<InstanceRecord>& out)
{
    const tinygltf::Node& n = model.nodes[nodeIndex];
    XMMATRIX world = XMMatrixMultiply(NodeLocalMatrix_Row(n), parentWorld);
    if (n.mesh >= 0 && n.mesh < static_cast<int>(model.meshes.size()))
    {
        const auto& m = model.meshes[n.mesh];
        for (size_t p = 0; p < m.primitives.size(); ++p)
        {
            uint32_t primIndex = UINT32_MAX;
            if (n.mesh < static_cast<int>(meshPrimToPrimIndex.size()))
            {
                const auto& vec = meshPrimToPrimIndex[n.mesh];
                if (p < vec.size())
                    primIndex = vec[p];
            }
            if (primIndex == UINT32_MAX)
                continue;
            InstanceRecord inst{};
            inst.primIndex = primIndex;
            XMStoreFloat4x4(&inst.world, world);
            out.push_back(inst);
        }
    }
    for (int child : n.children)
        if (child >= 0 && child < static_cast<int>(model.nodes.size()))
            GatherInstances_Recursive(model, child, meshPrimToPrimIndex, world, out);
}

static void BuildInstanceTable(const tinygltf::Model& model, const std::vector<PrimRecord>& prims, std::vector<InstanceRecord>& out)
{
    assert(!model.scenes.empty() && "glTF must contain at least one scene");
    std::vector<std::vector<uint32_t>> map;
    BuildMeshPrimToPrimIndex(prims, map, model.meshes.size());
    int sceneIndex = (model.defaultScene >= 0) ? model.defaultScene : 0;
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(model.scenes.size()))
        sceneIndex = 0;
    const XMMATRIX I = XMMatrixIdentity();
    for (int nodeIdx : model.scenes[sceneIndex].nodes)
        if (nodeIdx >= 0 && nodeIdx < static_cast<int>(model.nodes.size()))
            GatherInstances_Recursive(model, nodeIdx, map, I, out);
}

static void ResolveInstanceMaterialsAndSort(const std::vector<PrimRecord>& prims, const std::vector<MaterialRecord>& mats, std::vector<InstanceRecord>& inst)
{
    const uint32_t matCount = static_cast<uint32_t>(mats.empty() ? 1 : mats.size());

    for (auto& I : inst)
    {
        uint32_t mat = (I.primIndex < prims.size()) ? prims[I.primIndex].materialIndex : 0u;
        if (mat >= matCount)
            mat = 0u;
        I.materialIndex = mat;
    }

    std::stable_sort(inst.begin(), inst.end(), [](const InstanceRecord& a, const InstanceRecord& b) {
        if (a.materialIndex != b.materialIndex)
            return a.materialIndex < b.materialIndex;
        return a.primIndex < b.primIndex;
    });
}

static void ProcessAllMeshesAndWritePack(const fs::path& outPackPath, const fs::path& glbPath, const tinygltf::Model& model, bool fastCompress)
{
    std::vector<PrimRecord> prims;
    std::vector<Vertex> blobVertices;
    std::vector<uint32_t> blobIndices;
    std::vector<IskurMeshlet> blobMeshlets;
    std::vector<uint32_t> blobMLVerts;
    std::vector<uint8_t> blobMLTris;
    std::vector<MeshletBounds> blobMLBounds;

    for (int mi = 0; mi < static_cast<int>(model.meshes.size()); ++mi)
    {
        const auto& mesh = model.meshes[mi];
        for (int pi = 0; pi < static_cast<int>(mesh.primitives.size()); ++pi)
        {
            PrimRecord r = BuildOnePrimitive(model, mi, pi, blobVertices, blobIndices, blobMeshlets, blobMLVerts, blobMLTris, blobMLBounds);
            prims.push_back(r);
        }
    }

    std::vector<InstanceRecord> instTable;
    BuildInstanceTable(model, prims, instTable);

    std::vector<TextureRecord> texTable;
    std::vector<uint8_t> texBlob;
    {
        auto imgUsage = BuildImageUsageFlags(model);
        BuildTexturesToMemory(glbPath, model, imgUsage, fastCompress, texTable, texBlob);
        if (!model.textures.empty())
            assert(!texTable.empty() && "Texture table is empty despite glTF having textures");
    }

    std::vector<D3D12_SAMPLER_DESC> sampTable;
    std::vector<MaterialRecord> matTable;
    BuildSamplerTable(model, sampTable);
    BuildMaterialTable(model, texTable, matTable);

    if (!instTable.empty())
        ResolveInstanceMaterialsAndSort(prims, matTable, instTable);

    std::vector<ChunkRecord> chunks;
    uint32_t chunkCount = 7u + (texTable.empty() ? 0u : 2u) + (sampTable.empty() ? 0u : 1u) + (matTable.empty() ? 0u : 1u) + (instTable.empty() ? 0u : 1u);

    const uint64_t ofsHeader = 0;
    const uint64_t ofsChunkTbl = ofsHeader + sizeof(PackHeader);
    const uint64_t ofsPrimTbl = ofsChunkTbl + chunkCount * sizeof(ChunkRecord);
    const uint64_t ofsVertices = ofsPrimTbl + prims.size() * sizeof(PrimRecord);
    const uint64_t ofsIndices = ofsVertices + blobVertices.size() * sizeof(Vertex);
    const uint64_t ofsMeshlets = ofsIndices + blobIndices.size() * sizeof(uint32_t);
    const uint64_t ofsMLVerts = ofsMeshlets + blobMeshlets.size() * sizeof(IskurMeshlet);
    const uint64_t ofsMLTris = ofsMLVerts + blobMLVerts.size() * sizeof(uint32_t);
    const uint64_t ofsMLBounds = ofsMLTris + blobMLTris.size();
    uint64_t cur = ofsMLBounds + blobMLBounds.size() * sizeof(MeshletBounds);

    const uint64_t ofsTxHd = texTable.empty() ? 0 : cur;
    if (!texTable.empty())
        cur += texTable.size() * sizeof(TextureRecord);
    const uint64_t ofsTxTb = texTable.empty() ? 0 : cur;
    if (!texTable.empty())
        cur += texBlob.size();
    const uint64_t ofsSamp = sampTable.empty() ? 0 : cur;
    if (!sampTable.empty())
        cur += sampTable.size() * sizeof(D3D12_SAMPLER_DESC);
    const uint64_t ofsMatl = matTable.empty() ? 0 : cur;
    if (!matTable.empty())
        cur += matTable.size() * sizeof(MaterialRecord);
    const uint64_t ofsInst = instTable.empty() ? 0 : cur;
    if (!instTable.empty())
        cur += instTable.size() * sizeof(InstanceRecord);

    auto addChunk = [&](uint32_t id, uint64_t off, uint64_t size) {
        ChunkRecord r;
        r.id = id;
        r.offset = off;
        r.size = size;
        chunks.push_back(r);
    };

    addChunk(CH_PRIM, ofsPrimTbl, prims.size() * sizeof(PrimRecord));
    addChunk(CH_VERT, ofsVertices, blobVertices.size() * sizeof(Vertex));
    addChunk(CH_INDX, ofsIndices, blobIndices.size() * sizeof(uint32_t));
    addChunk(CH_MSHL, ofsMeshlets, blobMeshlets.size() * sizeof(IskurMeshlet));
    addChunk(CH_MLVT, ofsMLVerts, blobMLVerts.size() * sizeof(uint32_t));
    addChunk(CH_MLTR, ofsMLTris, blobMLTris.size());
    addChunk(CH_MLBD, ofsMLBounds, blobMLBounds.size() * sizeof(MeshletBounds));

    if (!texTable.empty())
    {
        addChunk(CH_TXHD, ofsTxHd, texTable.size() * sizeof(TextureRecord));
        addChunk(CH_TXTB, ofsTxTb, texBlob.size());
    }
    if (!sampTable.empty())
    {
        addChunk(CH_SAMP, ofsSamp, sampTable.size() * sizeof(D3D12_SAMPLER_DESC));
    }
    if (!matTable.empty())
    {
        addChunk(CH_MATL, ofsMatl, matTable.size() * sizeof(MaterialRecord));
    }
    if (!instTable.empty())
    {
        addChunk(CH_INST, ofsInst, instTable.size() * sizeof(InstanceRecord));
    }

    PackHeader hdr;
    std::memcpy(hdr.magic, "ISKURPACK", 9);
    hdr.version = 9;
    hdr.primCount = static_cast<uint32_t>(prims.size());
    hdr.chunkCount = static_cast<uint32_t>(chunks.size());
    hdr.chunkTableOffset = ofsChunkTbl;
    hdr.primTableOffset = ofsPrimTbl;
    hdr.verticesOffset = ofsVertices;
    hdr.indicesOffset = ofsIndices;
    hdr.meshletsOffset = ofsMeshlets;
    hdr.mlVertsOffset = ofsMLVerts;
    hdr.mlTrisOffset = ofsMLTris;
    hdr.mlBoundsOffset = ofsMLBounds;

    std::ofstream out(outPackPath, std::ios::binary | std::ios::trunc);
    assert(out && "Failed to open output pack file");

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(chunks.data()), static_cast<std::streamsize>(chunks.size() * sizeof(ChunkRecord)));
    out.write(reinterpret_cast<const char*>(prims.data()), static_cast<std::streamsize>(prims.size() * sizeof(PrimRecord)));
    out.write(reinterpret_cast<const char*>(blobVertices.data()), static_cast<std::streamsize>(blobVertices.size() * sizeof(Vertex)));
    out.write(reinterpret_cast<const char*>(blobIndices.data()), static_cast<std::streamsize>(blobIndices.size() * sizeof(uint32_t)));
    out.write(reinterpret_cast<const char*>(blobMeshlets.data()), static_cast<std::streamsize>(blobMeshlets.size() * sizeof(IskurMeshlet)));
    out.write(reinterpret_cast<const char*>(blobMLVerts.data()), static_cast<std::streamsize>(blobMLVerts.size() * sizeof(uint32_t)));
    out.write(reinterpret_cast<const char*>(blobMLTris.data()), static_cast<std::streamsize>(blobMLTris.size()));
    out.write(reinterpret_cast<const char*>(blobMLBounds.data()), static_cast<std::streamsize>(blobMLBounds.size() * sizeof(MeshletBounds)));
    if (!texTable.empty())
    {
        out.write(reinterpret_cast<const char*>(texTable.data()), static_cast<std::streamsize>(texTable.size() * sizeof(TextureRecord)));
        out.write(reinterpret_cast<const char*>(texBlob.data()), static_cast<std::streamsize>(texBlob.size()));
    }
    if (!sampTable.empty())
        out.write(reinterpret_cast<const char*>(sampTable.data()), static_cast<std::streamsize>(sampTable.size() * sizeof(D3D12_SAMPLER_DESC)));
    if (!matTable.empty())
        out.write(reinterpret_cast<const char*>(matTable.data()), static_cast<std::streamsize>(matTable.size() * sizeof(MaterialRecord)));
    if (!instTable.empty())
        out.write(reinterpret_cast<const char*>(instTable.data()), static_cast<std::streamsize>(instTable.size() * sizeof(InstanceRecord)));

    assert(out.good() && "Error writing pack file");

    std::println("Meshes pack written: {}", outPackPath.string());
    std::println("  prims={}, verts={}, inds={}, meshlets={}, mlVerts={}, mlTris={} bytes, mlBounds={}", static_cast<size_t>(hdr.primCount), blobVertices.size(), blobIndices.size(),
                 blobMeshlets.size(), blobMLVerts.size(), blobMLTris.size(), blobMLBounds.size());
    if (!texTable.empty())
        std::println("  textures: {}", texTable.size());
    if (!sampTable.empty())
        std::println("  samplers: {}", sampTable.size());
    if (!matTable.empty())
        std::println("  materials: {}", matTable.size());
    if (!instTable.empty())
        std::println("  instances: {} (sorted by material, then prim)", instTable.size());
}

static void PrintUsage()
{
    std::println("IskurScenePacker\nUsage:\n  IskurScenePacker --scene <scene> [--fast]\n  IskurScenePacker --all [--fast]");
}

static void WriteIskurScene(const fs::path& inGlb, const fs::path& outPack, bool fastCompress)
{
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string warn, err;
    bool ok = loader.LoadBinaryFromFile(&model, &warn, &err, inGlb.string());
    assert(ok && "Failed to load GLB with tinygltf");

    std::println("Loaded GLB: {}", inGlb.string());
    ProcessAllMeshesAndWritePack(outPack, inGlb, model, fastCompress);
}

int main(int argc, char** argv)
{
    auto t0 = std::chrono::steady_clock::now();

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    fs::path inSceneName;
    bool processAll = false;
    bool fast = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if ((a == "-i" || a == "--scene") && i + 1 < argc)
            inSceneName = argv[++i];
        else if (a == "--fast")
            fast = true;
        else if (a == "--all")
            processAll = true;
        else
        {
            assert(false && "Unknown command-line argument");
        }
    }

    if (processAll)
    {
        const fs::path srcRoot = fs::path("data") / "scenes_sources";
        const fs::path outRoot = fs::path("data") / "scenes";
        assert(fs::exists(srcRoot) && fs::is_directory(srcRoot) && "Sources directory must exist");
        if (!fs::exists(outRoot))
        {
            std::error_code ec;
            fs::create_directories(outRoot, ec);
            assert(!ec && "Failed to create output directory");
        }
        int total = 0, okc = 0, skipped = 0;
        for (const auto& de : fs::directory_iterator(srcRoot))
        {
            if (!de.is_regular_file() || de.path().extension() != ".glb")
                continue;
            std::string stem = de.path().stem().string();
            fs::path glb = de.path();
            fs::path pack = outRoot / (stem + ".iskurpack");
            if (!IsGlbFile(glb))
            {
                std::println("[skip] {} (not GLB)", stem);
                ++skipped;
                continue;
            }
            std::println("=== {} ===", stem);
            ++total;
            WriteIskurScene(glb, pack, fast);
            ++okc;
        }
        std::println("All-scenes: total={}, ok={}, skipped={} (fast={})", total, okc, skipped, fast ? "yes" : "no");
        CoUninitialize();

        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        std::println("Total time: {:.3f} s", sec);
        return 0;
    }

    if (inSceneName.empty())
    {
        PrintUsage();
        assert(false && "No scene specified; use --scene <name> or --all");
    }

    const std::string name = inSceneName.string();
    const fs::path glbPath = fs::path("data") / "scenes_sources" / (name + ".glb");
    const fs::path outPath = fs::path("data") / "scenes" / (name + ".iskurpack");

    assert(fs::exists(glbPath) && fs::is_regular_file(glbPath) && "GLB path does not exist or is not a file");
    assert(IsGlbFile(glbPath) && "Input file must be a GLB");

    {
        const auto outDir = outPath.parent_path();
        if (!fs::exists(outDir))
        {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            assert(!ec && "Failed to create output directory");
        }
    }

    WriteIskurScene(glbPath, outPath, fast);

    CoUninitialize();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::println("Total time: {:.3f} s", sec);

    return 0;
}
