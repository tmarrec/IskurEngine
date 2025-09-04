// Iškur Engine - Scene Packer
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#if __has_include("common/IskurPackFormat.h")
#include "common/IskurPackFormat.h"
#else
#include "../../common/IskurPackFormat.h"
#endif
#include <DirectXMath.h>
#include <DirectXTex.h>
#include <Objbase.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <fstream>
#include <meshoptimizer.h>
#include <mikktspace.h>
#include <mutex>
#include <numeric>
#include <string>
#include <tiny_gltf.h>
#include <unordered_map>
#include <vector>

using namespace DirectX;
namespace fs = std::filesystem;

// -------------------------------- PODs --------------------------------
struct IskurMeshlet
{
    uint32_t vertexOffset, triangleOffset;
    uint16_t vertexCount, triangleCount;
};

// Use shared pack format
using namespace IEPack;

// Pack structures and flags come from IEPack (see common/IskurPackFormat.h)

// ----------------------------- Matrix helpers -----------------------------
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
    float c0x = (1.f - 2.f * (yy + zz)) * S[0], c0y = (2.f * (xy + wz)) * S[0], c0z = (2.f * (xz - wy)) * S[0];
    float c1x = (2.f * (xy - wz)) * S[1], c1y = (1.f - 2.f * (xx + zz)) * S[1], c1z = (2.f * (yz + wx)) * S[1];
    float c2x = (2.f * (xz + wy)) * S[2], c2y = (2.f * (yz - wx)) * S[2], c2z = (1.f - 2.f * (xx + yy)) * S[2];
    M[0] = c0x;
    M[1] = c0y;
    M[2] = c0z;
    M[3] = 0;
    M[4] = c1x;
    M[5] = c1y;
    M[6] = c1z;
    M[7] = 0;
    M[8] = c2x;
    M[9] = c2y;
    M[10] = c2z;
    M[11] = 0;
    M[12] = T[0];
    M[13] = T[1];
    M[14] = T[2];
    M[15] = 1;
}
static inline XMMATRIX RowMatFromColArray(const float Mc[16])
{
    return XMMatrixSet(Mc[0], Mc[1], Mc[2], Mc[3], Mc[4], Mc[5], Mc[6], Mc[7], Mc[8], Mc[9], Mc[10], Mc[11], Mc[12], Mc[13], Mc[14], Mc[15]);
}
static XMMATRIX NodeLocalMatrix_Row(const tinygltf::Node& n)
{
    if (n.matrix.size() == 16)
    {
        // glTF stores this as column-major for column-vector math.
        // Convert to our row-major XMMATRIX the same way as TRS path.
        float Mc[16];
        for (int i = 0; i < 16; ++i)
            Mc[i] = (float)n.matrix[i];
        return RowMatFromColArray(Mc);
    }

    float T[3]{0, 0, 0}, S[3]{1, 1, 1}, Q[4]{0, 0, 0, 1};
    if (n.translation.size() == 3)
    {
        T[0] = (float)n.translation[0];
        T[1] = (float)n.translation[1];
        T[2] = (float)n.translation[2];
    }
    if (n.scale.size() == 3)
    {
        S[0] = (float)n.scale[0];
        S[1] = (float)n.scale[1];
        S[2] = (float)n.scale[2];
    }
    if (n.rotation.size() == 4)
    {
        Q[0] = (float)n.rotation[0];
        Q[1] = (float)n.rotation[1];
        Q[2] = (float)n.rotation[2];
        Q[3] = (float)n.rotation[3];
    }

    float Mc[16];
    TRS_ToColArray(T, Q, S, Mc);
    return RowMatFromColArray(Mc);
}
static float SpectralNorm_3x3_FromRowMat(const XMMATRIX& M)
{
    XMFLOAT4X4 F;
    XMStoreFloat4x4(&F, M);
    float c0x = F._11, c0y = F._21, c0z = F._31, c1x = F._12, c1y = F._22, c1z = F._32, c2x = F._13, c2y = F._23, c2z = F._33;
    auto L = [&](const float v[3], float o[3]) {
        o[0] = v[0] * c0x + v[1] * c1x + v[2] * c2x;
        o[1] = v[0] * c0y + v[1] * c1y + v[2] * c2y;
        o[2] = v[0] * c0z + v[1] * c1z + v[2] * c2z;
    };
    auto LT = [&](const float v[3], float o[3]) {
        o[0] = v[0] * c0x + v[1] * c0y + v[2] * c0z;
        o[1] = v[0] * c1x + v[1] * c1y + v[2] * c1z;
        o[2] = v[0] * c2x + v[1] * c2y + v[2] * c2z;
    };
    auto nrm = [](const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float x[3]{1, 0, 0};
    for (int i = 0; i < 8; ++i)
    {
        float y[3];
        L(x, y);
        float z[3];
        LT(y, z);
        float n = nrm(z);
        if (n > 0)
        {
            x[0] = z[0] / n;
            x[1] = z[1] / n;
            x[2] = z[2] / n;
        }
    }
    float y[3];
    L(x, y);
    return nrm(y);
}
static void TransformAABB_Row(const XMMATRIX& M, const float localMin[3], const float localMax[3], float outMin[3], float outMax[3], float outCenter[3])
{
    XMFLOAT4X4 F;
    XMStoreFloat4x4(&F, M);
    float cx = .5f * (localMin[0] + localMax[0]), cy = .5f * (localMin[1] + localMax[1]), cz = .5f * (localMin[2] + localMax[2]);
    float ex = .5f * (localMax[0] - localMin[0]), ey = .5f * (localMax[1] - localMin[1]), ez = .5f * (localMax[2] - localMin[2]);
    float c0x = F._11, c0y = F._21, c0z = F._31, c1x = F._12, c1y = F._22, c1z = F._32, c2x = F._13, c2y = F._23, c2z = F._33, tx = F._41, ty = F._42, tz = F._43;
    float wcx = cx * c0x + cy * c1x + cz * c2x + tx, wcy = cx * c0y + cy * c1y + cz * c2y + ty, wcz = cx * c0z + cy * c1z + cz * c2z + tz;
    float exw = std::fabs(c0x) * ex + std::fabs(c1x) * ey + std::fabs(c2x) * ez, eyw = std::fabs(c0y) * ex + std::fabs(c1y) * ey + std::fabs(c2y) * ez,
          ezw = std::fabs(c0z) * ex + std::fabs(c1z) * ey + std::fabs(c2z) * ez;
    outMin[0] = wcx - exw;
    outMax[0] = wcx + exw;
    outMin[1] = wcy - eyw;
    outMax[1] = wcy + eyw;
    outMin[2] = wcz - ezw;
    outMax[2] = wcz + ezw;
    outCenter[0] = wcx;
    outCenter[1] = wcy;
    outCenter[2] = wcz;
}

// ----------------------------- IO helpers -----------------------------
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
        if (ti < 0 || ti >= int(model.textures.size()))
            return;
        int img = model.textures[size_t(ti)].source;
        if (img < 0 || img >= int(flags.size()))
            return;
        flags[size_t(img)] |= f;
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
    return uint32_t(tinygltf::GetNumComponentsInType(a.type) * tinygltf::GetComponentSizeInBytes(a.componentType));
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
    auto* ud = (const MikkUserData*)ctx->m_pUserData;
    return int(ud->indices->size() / 3);
}
static void mk_getPosition(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = (const MikkUserData*)ctx->m_pUserData;
    uint32_t i = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    auto& P = (*ud->verts)[i].position;
    out[0] = P.x;
    out[1] = P.y;
    out[2] = P.z;
}
static void mk_getNormal(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = (const MikkUserData*)ctx->m_pUserData;
    uint32_t i = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    auto& N = (*ud->verts)[i].normal;
    out[0] = N.x;
    out[1] = N.y;
    out[2] = N.z;
}
static void mk_getTexCoord(const SMikkTSpaceContext* ctx, float out[2], const int f, const int v)
{
    auto* ud = (const MikkUserData*)ctx->m_pUserData;
    uint32_t i = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    auto& uv = (*ud->verts)[i].texCoord;
    out[0] = uv.x;
    out[1] = uv.y;
}
static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const float t[3], const float s, const int f, const int v)
{
    auto* ud = (MikkUserData*)ctx->m_pUserData;
    uint32_t i = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    auto& d = (*ud->verts)[i].tangent;
    d.x = t[0];
    d.y = t[1];
    d.z = t[2];
    d.w = -s;
}

// ----------------------------- Options -----------------------------
struct RunOptions
{
    bool fast = false;
};

// ---------------------- Texture embed (TXHD/TXTB) ----------------------
static bool BuildTexturesToMemory(const fs::path& glbPath, const tinygltf::Model& model, const std::vector<uint32_t>& imgUsage, bool fastCompress, std::vector<TextureRecord>& outTable,
                                  std::vector<uint8_t>& outBlob)
{
    auto kb = [](uint64_t b) { return double(b) / 1024.0; };
    auto fmtName = [](DXGI_FORMAT f) -> const char* {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return "RGBA8_sRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "BGRA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return "BGRA8_sRGB";
        case DXGI_FORMAT_BC5_UNORM:
            return "BC5";
        case DXGI_FORMAT_BC7_UNORM:
            return "BC7";
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return "BC7_sRGB";
        default:
            return "DXGI_FMT";
        }
    };
    auto usageStr = [](uint32_t f) {
        std::string s;
        if (f & IMG_BASECOLOR)
            s += "BC,";
        if (f & IMG_NORMAL)
            s += "NRM,";
        if (f & IMG_METALROUGH)
            s += "MR,";
        if (f & IMG_OCCLUSION)
            s += "OCC,";
        if (f & IMG_EMISSIVE)
            s += "EMS,";
        if (!s.empty())
            s.pop_back();
        return s;
    };

    outTable.clear();
    outBlob.clear();
    std::vector<uint8_t> done(model.images.size(), 0);
    const fs::path baseDir = glbPath.parent_path();

    const size_t totalTex = model.textures.size();
    const size_t totalImg = model.images.size();

    auto loadImageBytes = [&](size_t imgIndex, const uint8_t*& bytes, size_t& size, std::vector<uint8_t>& owned) -> bool {
        const auto& img = model.images[imgIndex];
        if (img.bufferView >= 0)
        {
            const auto& bv = model.bufferViews[size_t(img.bufferView)];
            const auto& buf = model.buffers[size_t(bv.buffer)];
            if (size_t(bv.byteOffset) + size_t(bv.byteLength) > buf.data.size())
                return false;
            bytes = buf.data.data() + size_t(bv.byteOffset);
            size = size_t(bv.byteLength);
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
            size_t sz = size_t(f.tellg());
            f.seekg(0, std::ios::beg);
            owned.resize(sz);
            f.read((char*)owned.data(), sz);
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
        if (imgIdx < 0 || imgIdx >= int(model.images.size()))
            continue;

        size_t i = size_t(imgIdx);
        if (done[i])
            continue; // already baked (image shared by multiple textures)

        // Context for logging
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

        // Count how many glTF textures reference this image (info)
        size_t refCount = 0;
        for (size_t ti = 0; ti < model.textures.size(); ++ti)
            if (model.textures[ti].source == int(i))
                ++refCount;

        const uint32_t use = (i < imgUsage.size()) ? imgUsage[i] : 0u;

        const uint8_t* raw = nullptr;
        size_t rawSize = 0;
        std::vector<uint8_t> owned;

        if (!loadImageBytes(i, raw, rawSize, owned))
        {
            std::fprintf(stderr, "[tex %zu/%zu][img %zu/%zu] read fail (src=%s)\n", t, totalTex, i, totalImg, srcDesc.c_str());
            return false;
        }

        std::printf("[tex %zu/%zu][img %zu/%zu] refs=%zu usage={%s} src=\"%s\" raw=%.1f KiB\n", t, totalTex, i, totalImg, refCount, usageStr(use).c_str(), srcDesc.c_str(), kb(uint64_t(rawSize)));

        ScratchImage loaded;
        if (FAILED(LoadAnyImageMemory(raw, rawSize, loaded)))
        {
            std::fprintf(stderr, "[tex %zu/%zu][img %zu/%zu] decode fail\n", t, totalTex, i, totalImg);
            return false;
        }

        TexMetadata meta0 = loaded.GetMetadata();
        std::printf("  - decoded: %zux%zu, mips=%zu, fmt=%s\n", meta0.width, meta0.height, meta0.mipLevels, fmtName(meta0.format));

        bool isNormal = (i < imgUsage.size()) && (imgUsage[i] & IMG_NORMAL);
        bool isSRGB = !isNormal && ((i < imgUsage.size()) && ((imgUsage[i] & (IMG_BASECOLOR | IMG_EMISSIVE)) && !(imgUsage[i] & (IMG_METALROUGH | IMG_OCCLUSION))));
        DXGI_FORMAT wantBase = isSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

        const Image* srcImages = nullptr;
        size_t srcCount = 0;
        TexMetadata meta{};
        ScratchImage converted;

        bool didConvert = (loaded.GetMetadata().format != wantBase);
        if (didConvert)
        {
            if (FAILED(Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(), wantBase, TEX_FILTER_DEFAULT, 0.0f, converted)))
            {
                std::fprintf(stderr, "[tex %zu/%zu][img %zu/%zu] convert fail (%s -> %s)\n", t, totalTex, i, totalImg, fmtName(loaded.GetMetadata().format), fmtName(wantBase));
                return false;
            }
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

        if (didConvert)
            std::printf("  - convert: %s -> %s\n", fmtName(loaded.GetMetadata().format), fmtName(wantBase));

        ScratchImage mip;
        if (SUCCEEDED(GenerateMipMaps(srcImages, srcCount, meta, TEX_FILTER_DEFAULT, 0, mip)))
        {
            srcImages = mip.GetImages();
            srcCount = mip.GetImageCount();
            meta = mip.GetMetadata();
        }
        std::printf("  - mips: %zu levels\n", meta.mipLevels);

        DXGI_FORMAT compFmt = isNormal ? DXGI_FORMAT_BC5_UNORM : (isSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM);
        TEX_COMPRESS_FLAGS comp = TEX_COMPRESS_PARALLEL;
        if (!isNormal && fastCompress)
            comp |= TEX_COMPRESS_BC7_QUICK;

        ScratchImage bc;
        if (FAILED(Compress(srcImages, srcCount, meta, compFmt, comp, 0.5f, bc)))
        {
            std::fprintf(stderr, "[tex %zu/%zu][img %zu/%zu] BC compress fail\n", t, totalTex, i, totalImg);
            return false;
        }

        Blob blob;
        if (FAILED(SaveToDDSMemory(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_FORCE_DX10_EXT, blob)))
        {
            std::fprintf(stderr, "[tex %zu/%zu][img %zu/%zu] save DDS fail\n", t, totalTex, i, totalImg);
            return false;
        }

        std::printf("  - compress: %s%s flags=[%s%s]\n", fmtName(compFmt), (fastCompress && !isNormal) ? " (quick)" : "", isSRGB ? "SRGB" : "LIN", isNormal ? "|NORM" : "");
        std::printf("  - dds: %.1f KiB\n", kb(uint64_t(blob.GetBufferSize())));

        uint64_t off = uint64_t(outBlob.size()), sz = uint64_t(blob.GetBufferSize());
        outBlob.insert(outBlob.end(), (const uint8_t*)blob.GetBufferPointer(), (const uint8_t*)blob.GetBufferPointer() + sz);

        TextureRecord tr{};
        tr.imageIndex = (uint32_t)i;
        tr.flags = (isSRGB ? TEXFLAG_SRGB : 0) | (isNormal ? TEXFLAG_NORMAL : 0);
        tr.byteOffset = off;
        tr.byteSize = sz;
        outTable.push_back(tr);
        done[i] = 1;
    }

    if (!model.textures.empty() && outTable.empty())
        return false;
    return true;
}

// ---------------------- Materials & samplers ----------------------
static uint32_t MapWrapModeGLTFToD3D12Addr(int wrap)
{
    switch (wrap)
    {
    case 33648:
        return D3D12_TAM_MIRROR;
    case 33071:
        return D3D12_TAM_CLAMP;
    default:
        return D3D12_TAM_WRAP;
    }
}
static uint32_t BakeD3D12Filter(int minFilter, int magFilter, float maxAniso)
{
    if (maxAniso > 1.0f)
        return D3D12_FILTER_ANISOTROPIC;
    if (minFilter <= 0)
        minFilter = 9987;
    if (magFilter <= 0)
        magFilter = 9729;
    if (minFilter == 9987)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    if (minFilter == 9985 || minFilter == 9986 || magFilter == 9729)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    return D3D12_FILTER_MIN_MAG_MIP_POINT;
}
static float ReadSamplerAnisotropy(const tinygltf::Sampler& s)
{
    auto it = s.extensions.find("EXT_texture_filter_anisotropic");
    if (it == s.extensions.end() || !it->second.IsObject())
        return 1.0f;
    auto jt = it->second.Get("anisotropy");
    if (jt.IsNumber())
    {
        double v = jt.GetNumberAsDouble();
        v = std::clamp(v, 1.0, 16.0);
        return (float)v;
    }
    return 1.0f;
}
static void BuildSamplerTable(const tinygltf::Model& model, std::vector<SamplerDisk>& outSamp, std::vector<uint32_t>& outGlTfToSamp)
{
    outSamp.clear();
    outGlTfToSamp.assign(model.samplers.size(), 0u);
    struct Key
    {
        uint32_t filt, u, v, w;
        float bias, minLod, maxLod;
        uint32_t aniso, cmp;
        float border[4];
        bool operator==(const Key& o) const
        {
            return filt == o.filt && u == o.u && v == o.v && w == o.w && bias == o.bias && minLod == o.minLod && maxLod == o.maxLod && aniso == o.aniso && cmp == o.cmp && border[0] == o.border[0] &&
                   border[1] == o.border[1] && border[2] == o.border[2] && border[3] == o.border[3];
        }
    };
    std::vector<Key> keys;
    auto pushUnique = [&](const Key& k) -> uint32_t {
        for (uint32_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k)
                return i;
        keys.push_back(k);
        SamplerDisk r{};
        r.d3d12Filter = k.filt;
        r.addressU = k.u;
        r.addressV = k.v;
        r.addressW = k.w;
        r.mipLODBias = k.bias;
        r.minLOD = k.minLod;
        r.maxLOD = k.maxLod;
        r.maxAnisotropy = k.aniso;
        r.comparisonFunc = k.cmp;
        std::memcpy(r.borderColor, k.border, 16);
        outSamp.push_back(r);
        return uint32_t(outSamp.size() - 1);
    };
    Key def{};
    def.filt = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    def.u = def.v = def.w = D3D12_TAM_WRAP;
    def.bias = 0;
    def.minLod = 0;
    def.maxLod = 1000;
    def.aniso = 1;
    def.cmp = D3D12_CF_NEVER;
    def.border[0] = def.border[1] = def.border[2] = 0;
    def.border[3] = 1;
    pushUnique(def);
    for (const auto& s : model.samplers)
    {
        uint32_t aniso = (uint32_t)std::clamp((int)std::lround(ReadSamplerAnisotropy(s)), 1, 16);
        Key k{};
        k.filt = BakeD3D12Filter(s.minFilter, s.magFilter, (float)aniso);
        k.u = MapWrapModeGLTFToD3D12Addr(s.wrapS);
        k.v = MapWrapModeGLTFToD3D12Addr(s.wrapT);
        k.w = k.v;
        k.bias = 0;
        k.minLod = 0;
        k.maxLod = 1000;
        k.aniso = aniso;
        k.cmp = D3D12_CF_NEVER;
        k.border[0] = k.border[1] = k.border[2] = 0;
        k.border[3] = 1;
        outGlTfToSamp[&s - &model.samplers[0]] = pushUnique(k);
    }
}
static int MapTextureToTxhdIndex(const tinygltf::Model& model, int gltfTexIndex, const std::vector<int>& txhdByImage)
{
    if (gltfTexIndex < 0 || gltfTexIndex >= int(model.textures.size()))
        return -1;
    int img = model.textures[size_t(gltfTexIndex)].source;
    if (img < 0 || img >= int(txhdByImage.size()))
        return -1;
    return txhdByImage[size_t(img)];
}
static void BuildMaterialTable(const tinygltf::Model& model, const std::vector<TextureRecord>& texTable, std::vector<MaterialRecord>& outMatl, const std::vector<SamplerDisk>&,
                               const std::vector<uint32_t>& gltfSamplerToSamp, std::vector<int>& outTxhdByImage)
{
    outTxhdByImage.assign(model.images.size(), -1);
    for (size_t i = 0; i < texTable.size(); ++i)
        if (texTable[i].imageIndex < outTxhdByImage.size())
            outTxhdByImage[texTable[i].imageIndex] = (int)i;
    size_t matCount = std::max<size_t>(1, model.materials.size());
    outMatl.assign(matCount, MaterialRecord{});
    auto texInfoToTxhd = [&](const tinygltf::TextureInfo& ti) -> int { return MapTextureToTxhdIndex(model, ti.index, outTxhdByImage); };
    auto parseAlphaMode = [](const tinygltf::Material& m) -> uint32_t {
        if (m.alphaMode == "MASK")
            return MATF_ALPHA_MASK;
        if (m.alphaMode == "BLEND")
            return MATF_ALPHA_BLEND;
        return 0;
    };
    for (size_t i = 0; i < matCount; ++i)
    {
        MaterialRecord mr{};
        mr.baseColorTx = mr.normalTx = mr.metallicRoughTx = mr.occlusionTx = mr.emissiveTx = -1;
        mr.baseColorSampler = mr.normalSampler = mr.metallicRoughSampler = mr.occlusionSampler = mr.emissiveSampler = UINT32_MAX;
        mr.baseColorFactor[0] = mr.baseColorFactor[1] = mr.baseColorFactor[2] = mr.baseColorFactor[3] = 1;
        mr.emissiveFactor[0] = mr.emissiveFactor[1] = mr.emissiveFactor[2] = 0;
        mr.metallicFactor = 1;
        mr.roughnessFactor = 1;
        mr.normalScale = 1;
        mr.occlusionStrength = 1;
        mr.alphaCutoff = .5f;
        mr.uvScale[0] = mr.uvScale[1] = 1;
        mr.uvOffset[0] = mr.uvOffset[1] = 0;
        mr.uvRotation = 0;
        mr.flags = 0;
        mr._pad1 = 0;
        if (!model.materials.empty())
        {
            const auto& m = model.materials[i];
            if (m.pbrMetallicRoughness.baseColorFactor.size() == 4)
                for (int k = 0; k < 4; ++k)
                    mr.baseColorFactor[k] = (float)m.pbrMetallicRoughness.baseColorFactor[k];
            mr.metallicFactor = (float)m.pbrMetallicRoughness.metallicFactor;
            mr.roughnessFactor = (float)m.pbrMetallicRoughness.roughnessFactor;
            if (m.emissiveFactor.size() == 3)
            {
                mr.emissiveFactor[0] = (float)m.emissiveFactor[0];
                mr.emissiveFactor[1] = (float)m.emissiveFactor[1];
                mr.emissiveFactor[2] = (float)m.emissiveFactor[2];
            }
            if (m.alphaMode == "MASK")
                mr.alphaCutoff = (float)m.alphaCutoff;
            mr.flags |= parseAlphaMode(m);
            if (m.doubleSided)
                mr.flags |= MATF_DOUBLE_SIDED;
            auto chooseSampler = [&](int texIndex) -> uint32_t {
                if (texIndex < 0 || texIndex >= (int)model.textures.size())
                    return 0u;
                int s = model.textures[size_t(texIndex)].sampler;
                if (s < 0)
                    return 0u;
                if (s < (int)gltfSamplerToSamp.size())
                    return gltfSamplerToSamp[size_t(s)];
                return 0u;
            };
            if (m.pbrMetallicRoughness.baseColorTexture.index >= 0)
            {
                mr.baseColorTx = texInfoToTxhd(m.pbrMetallicRoughness.baseColorTexture);
                if (mr.baseColorTx >= 0)
                {
                    mr.flags |= MATF_HAS_BC;
                    mr.baseColorSampler = chooseSampler(m.pbrMetallicRoughness.baseColorTexture.index);
                }
            }
            if (m.normalTexture.index >= 0)
            {
                mr.normalTx = MapTextureToTxhdIndex(model, m.normalTexture.index, outTxhdByImage);
                mr.normalScale = (float)m.normalTexture.scale;
                if (mr.normalTx >= 0)
                {
                    mr.flags |= MATF_HAS_NORM;
                    mr.normalSampler = chooseSampler(m.normalTexture.index);
                }
            }
            if (m.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
            {
                mr.metallicRoughTx = texInfoToTxhd(m.pbrMetallicRoughness.metallicRoughnessTexture);
                if (mr.metallicRoughTx >= 0)
                {
                    mr.flags |= MATF_HAS_MR;
                    mr.metallicRoughSampler = chooseSampler(m.pbrMetallicRoughness.metallicRoughnessTexture.index);
                }
            }
            if (m.occlusionTexture.index >= 0)
            {
                mr.occlusionTx = MapTextureToTxhdIndex(model, m.occlusionTexture.index, outTxhdByImage);
                mr.occlusionStrength = (float)m.occlusionTexture.strength;
                if (mr.occlusionTx >= 0)
                {
                    mr.flags |= MATF_HAS_OCC;
                    mr.occlusionSampler = chooseSampler(m.occlusionTexture.index);
                }
            }
            if (m.emissiveTexture.index >= 0)
            {
                mr.emissiveTx = MapTextureToTxhdIndex(model, m.emissiveTexture.index, outTxhdByImage);
                if (mr.emissiveTx >= 0)
                {
                    mr.flags |= MATF_HAS_EMISSIVE;
                    mr.emissiveSampler = chooseSampler(m.emissiveTexture.index);
                }
            }
        }
        outMatl[i] = mr;
    }
}

// ---------------------- Geometry build ----------------------
struct BuiltPrimitive
{
    uint32_t meshIndex = 0, primIndex = 0, materialIndex = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<IskurMeshlet> meshlets;
    std::vector<uint32_t> mlVerts;
    std::vector<uint8_t> mlTris;
    std::vector<MeshletBounds> mlBounds;
};

static void BuildOnePrimitive(const tinygltf::Model& model, int meshIdx, int primIdx, BuiltPrimitive& out)
{
    const auto& gltfPrim = model.meshes[size_t(meshIdx)].primitives[size_t(primIdx)];
    if (gltfPrim.mode != TINYGLTF_MODE_TRIANGLES)
    {
        std::fprintf(stderr, "Non-triangle primitive.\n");
        std::abort();
    }
    out.meshIndex = (uint32_t)meshIdx;
    out.primIndex = (uint32_t)primIdx;
    out.materialIndex = (gltfPrim.material >= 0) ? (uint32_t)gltfPrim.material : 0;

    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;

    if (gltfPrim.attributes.contains("NORMAL"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("NORMAL"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = (uint32_t)acc.count, stride = AccessorStrideBytes(acc, view);
        normals.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(&normals[i], data + i * stride, sizeof(XMFLOAT3));
    }
    if (gltfPrim.attributes.contains("TEXCOORD_0"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("TEXCOORD_0"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = (uint32_t)acc.count, stride = AccessorStrideBytes(acc, view);
        texcoords.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(&texcoords[i], data + i * stride, sizeof(XMFLOAT2));
    }

    std::vector<Vertex> initialVertices;
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("POSITION"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = (uint32_t)acc.count, stride = AccessorStrideBytes(acc, view);
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
                v.normal = XMFLOAT3(0, 0, 0);
            v.texCoord = (!texcoords.empty()) ? texcoords[i] : XMFLOAT2(0, 0);
            v.tangent = XMFLOAT4(0, 0, 0, 0);
            initialVertices[i] = v;
        }
    }

    std::vector<uint32_t> initialIndices;
    if (gltfPrim.indices >= 0)
    {
        const auto& acc = model.accessors[size_t(gltfPrim.indices)];
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        uint32_t count = (uint32_t)acc.count;
        initialIndices.resize(count);
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = uint32_t(data[i]);
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const uint16_t* p = (const uint16_t*)data;
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = (uint32_t)p[i];
        }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const uint32_t* p = (const uint32_t*)data;
            for (uint32_t i = 0; i < count; ++i)
                initialIndices[i] = p[i];
        }
        else
        {
            std::fprintf(stderr, "Unsupported index type.\n");
            std::abort();
        }
    }
    else
    {
        initialIndices.resize(initialVertices.size());
        for (uint32_t i = 0; i < (uint32_t)initialVertices.size(); ++i)
            initialIndices[i] = i;
    }

    if (normals.empty())
    {
        for (size_t i = 0; i < initialIndices.size(); i += 3)
        {
            uint32_t i0 = initialIndices[i + 0], i1 = initialIndices[i + 1], i2 = initialIndices[i + 2];
            const XMFLOAT3 &v0 = initialVertices[i0].position, &v1 = initialVertices[i1].position, &v2 = initialVertices[i2].position;
            float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z, e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x, len = std::sqrt(nx * nx + ny * ny + nz * nz);
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
    out.indices.swap(outIndices);
    out.vertices.swap(outVertices);

    constexpr uint64_t maxVertices = 64, maxTriangles = 124;
    constexpr float coneWeight = 0.f;
    const uint32_t maxMeshlets = (uint32_t)meshopt_buildMeshletsBound(out.indices.size(), maxVertices, maxTriangles);
    std::vector<meshopt_Meshlet> temp(maxMeshlets);
    out.mlVerts.resize(maxMeshlets * maxVertices);
    out.mlTris.resize(maxMeshlets * maxTriangles * 3);
    uint32_t mc = (uint32_t)meshopt_buildMeshlets(temp.data(), out.mlVerts.data(), out.mlTris.data(), out.indices.data(), out.indices.size(), &out.vertices[0].position.x, out.vertices.size(),
                                                  sizeof(Vertex), maxVertices, maxTriangles, coneWeight);
    temp.resize(mc);
    if (!temp.empty())
    {
        const auto& last = temp.back();
        out.mlVerts.resize(last.vertex_offset + last.vertex_count);
        out.mlTris.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));
    }

    out.mlBounds.clear();
    out.mlBounds.reserve(temp.size());
    for (const auto& m : temp)
    {
        meshopt_optimizeMeshlet(&out.mlVerts[m.vertex_offset], &out.mlTris[m.triangle_offset], m.triangle_count, m.vertex_count);
        meshopt_Bounds b =
            meshopt_computeMeshletBounds(&out.mlVerts[m.vertex_offset], &out.mlTris[m.triangle_offset], m.triangle_count, &out.vertices[0].position.x, out.vertices.size(), sizeof(Vertex));
        MeshletBounds mb{};
        mb.center = XMFLOAT3(b.center[0], b.center[1], b.center[2]);
        mb.radius = b.radius;
        mb.cone_apex = XMFLOAT3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
        mb.cone_axis = XMFLOAT3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
        mb.cone_cutoff = b.cone_cutoff;
        mb.coneAxisAndCutoff =
            (uint32_t(uint8_t(b.cone_axis_s8[0]))) | (uint32_t(uint8_t(b.cone_axis_s8[1])) << 8) | (uint32_t(uint8_t(b.cone_axis_s8[2])) << 16) | (uint32_t(uint8_t(b.cone_cutoff_s8)) << 24);
        out.mlBounds.push_back(mb);
    }
    out.meshlets.resize(temp.size());
    for (size_t i = 0; i < temp.size(); ++i)
    {
        out.meshlets[i].vertexOffset = temp[i].vertex_offset;
        out.meshlets[i].triangleOffset = temp[i].triangle_offset;
        out.meshlets[i].vertexCount = (uint16_t)temp[i].vertex_count;
        out.meshlets[i].triangleCount = (uint16_t)temp[i].triangle_count;
    }
}

struct LocalAabb
{
    float minv[3], maxv[3], halfLen;
};
static void ComputeLocalAabbsFromBuilt(const std::vector<BuiltPrimitive>& built, std::vector<LocalAabb>& out)
{
    out.resize(built.size());
    for (size_t i = 0; i < built.size(); ++i)
    {
        const auto& b = built[i];
        float mn[3] = {+FLT_MAX, +FLT_MAX, +FLT_MAX}, mx[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& v : b.vertices)
        {
            mn[0] = min(mn[0], v.position.x);
            mn[1] = min(mn[1], v.position.y);
            mn[2] = min(mn[2], v.position.z);
            mx[0] = max(mx[0], v.position.x);
            mx[1] = max(mx[1], v.position.y);
            mx[2] = max(mx[2], v.position.z);
        }
        out[i].minv[0] = mn[0];
        out[i].minv[1] = mn[1];
        out[i].minv[2] = mn[2];
        out[i].maxv[0] = mx[0];
        out[i].maxv[1] = mx[1];
        out[i].maxv[2] = mx[2];
        float hx = .5f * (mx[0] - mn[0]), hy = .5f * (mx[1] - mn[1]), hz = .5f * (mx[2] - mn[2]);
        out[i].halfLen = std::sqrt(hx * hx + hy * hy + hz * hz);
    }
}
static void BuildMeshPrimToPrimIndex(const std::vector<PrimRecord>& prims, std::vector<std::vector<uint32_t>>& map, size_t meshCount)
{
    map.clear();
    map.resize(meshCount);
    for (uint32_t pi = 0; pi < prims.size(); ++pi)
    {
        const auto& pr = prims[pi];
        auto& vec = map[pr.meshIndex];
        if (vec.size() <= pr.primIndex)
            vec.resize(size_t(pr.primIndex) + 1, UINT32_MAX);
        vec[pr.primIndex] = pi;
    }
}

static void GatherInstances_Recursive(const tinygltf::Model& model, int nodeIndex, const std::vector<std::vector<uint32_t>>& meshPrimToPrimIndex, const std::vector<LocalAabb>& localAabbs,
                                      const XMMATRIX& parentWorld, std::vector<InstanceRecord>& out)
{
    const tinygltf::Node& n = model.nodes[size_t(nodeIndex)];
    XMMATRIX world = XMMatrixMultiply(NodeLocalMatrix_Row(n), parentWorld);
    if (n.mesh >= 0 && n.mesh < int(model.meshes.size()))
    {
        const auto& m = model.meshes[size_t(n.mesh)];
        for (size_t p = 0; p < m.primitives.size(); ++p)
        {
            uint32_t primIndex = UINT32_MAX;
            if (size_t(n.mesh) < meshPrimToPrimIndex.size())
            {
                const auto& vec = meshPrimToPrimIndex[size_t(n.mesh)];
                if (p < vec.size())
                    primIndex = vec[p];
            }
            if (primIndex == UINT32_MAX)
                continue;
            const auto& la = localAabbs[primIndex];
            InstanceRecord inst{};
            inst.primIndex = primIndex;
            inst.materialOverride = UINT32_MAX;
            float wMin[3], wMax[3], wCtr[3];
            TransformAABB_Row(world, la.minv, la.maxv, wMin, wMax, wCtr);
            inst.aabbMin[0] = wMin[0];
            inst.aabbMin[1] = wMin[1];
            inst.aabbMin[2] = wMin[2];
            inst.aabbMax[0] = wMax[0];
            inst.aabbMax[1] = wMax[1];
            inst.aabbMax[2] = wMax[2];
            inst.bsCenter[0] = wCtr[0];
            inst.bsCenter[1] = wCtr[1];
            inst.bsCenter[2] = wCtr[2];
            inst.maxScale = SpectralNorm_3x3_FromRowMat(world);
            inst.bsRadius = inst.maxScale * la.halfLen;
            XMStoreFloat4x4(&inst.world, world);
            out.push_back(inst);
        }
    }
    for (int child : n.children)
        if (child >= 0 && child < (int)model.nodes.size())
            GatherInstances_Recursive(model, child, meshPrimToPrimIndex, localAabbs, world, out);
}
static void BuildInstanceTable(const tinygltf::Model& model, const std::vector<PrimRecord>& prims, const std::vector<BuiltPrimitive>& built, std::vector<InstanceRecord>& out)
{
    out.clear();
    if (model.scenes.empty())
        return;
    std::vector<LocalAabb> localAabbs;
    ComputeLocalAabbsFromBuilt(built, localAabbs);
    std::vector<std::vector<uint32_t>> map;
    BuildMeshPrimToPrimIndex(prims, map, model.meshes.size());
    int sceneIndex = (model.defaultScene >= 0) ? model.defaultScene : 0;
    if (sceneIndex < 0 || sceneIndex >= (int)model.scenes.size())
        sceneIndex = 0;
    const XMMATRIX I = XMMatrixIdentity();
    for (int nodeIdx : model.scenes[size_t(sceneIndex)].nodes)
        if (nodeIdx >= 0 && nodeIdx < (int)model.nodes.size())
            GatherInstances_Recursive(model, nodeIdx, map, localAabbs, I, out);
}

// ---------------------- Draw list builder ----------------------
enum : uint32_t
{
    PIPELINE_OPAQUE = 0,
    PIPELINE_MASKED = 1,
    PIPELINE_BLENDED = 2
};
using DrawItem = IEPack::DrawItem; // on-disk draw item layout
static inline uint32_t PipelineFromFlags(uint32_t f)
{
    if (f & MATF_ALPHA_BLEND)
        return PIPELINE_BLENDED;
    if (f & MATF_ALPHA_MASK)
        return PIPELINE_MASKED;
    return PIPELINE_OPAQUE;
}
static inline bool IsNoCullFromFlags(uint32_t f)
{
    return (f & MATF_DOUBLE_SIDED) != 0;
}
static uint64_t MakeMaterialSortKey(uint32_t pipeline, const MaterialRecord& m, uint32_t materialIndex)
{
    uint64_t key = 0;
    key |= (uint64_t(pipeline & 0x3) << 62);
    uint32_t variant = 0;
    if (m.flags & MATF_HAS_NORM)
        variant |= 1 << 0;
    if (m.flags & MATF_HAS_MR)
        variant |= 1 << 1;
    if (m.flags & MATF_HAS_OCC)
        variant |= 1 << 2;
    if (m.flags & MATF_HAS_EMISSIVE)
        variant |= 1 << 3;
    if (m.flags & MATF_DOUBLE_SIDED)
        variant |= 1 << 4;
    key |= (uint64_t(variant & 0xFF) << 48);
    key |= (uint64_t(materialIndex & 0xFFFF) << 32);
    return key;
}
struct DrawListsBuilt
{
    std::vector<DrawItem> itemsOpaqueCull;
    std::vector<uint32_t> instOpaqueCull;
    std::vector<DrawItem> itemsOpaqueNoCull;
    std::vector<uint32_t> instOpaqueNoCull;
    std::vector<DrawItem> itemsMaskedCull;
    std::vector<uint32_t> instMaskedCull;
    std::vector<DrawItem> itemsMaskedNoCull;
    std::vector<uint32_t> instMaskedNoCull;
    std::vector<DrawItem> itemsBlendedCull;
    std::vector<uint32_t> instBlendedCull;
    std::vector<DrawItem> itemsBlendedNoCull;
    std::vector<uint32_t> instBlendedNoCull;
};
static void BuildDrawLists(const std::vector<PrimRecord>& prims, const std::vector<InstanceRecord>& inst, const std::vector<MaterialRecord>& mats, DrawListsBuilt& outDL)
{
    outDL = {};
    if (prims.empty() || inst.empty() || mats.empty())
        return;
    struct AggKey
    {
        uint32_t prim, mat;
        uint8_t pipe, nocull;
    };
    auto keyToU64 = [](const AggKey& k) -> uint64_t {
        return (uint64_t(k.pipe) << 56) | (uint64_t(k.nocull) << 55) | (uint64_t(k.mat & ((1u << 27) - 1)) << 28) | uint64_t(k.prim & ((1u << 28) - 1));
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
    buckets.reserve(inst.size() * 2);
    for (uint32_t iid = 0; iid < inst.size(); ++iid)
    {
        const auto& I = inst[iid];
        if (I.primIndex >= prims.size())
            continue;
        uint32_t mat = prims[I.primIndex].materialIndex;
        if (I.materialOverride != UINT32_MAX)
            mat = I.materialOverride;
        if (mat >= mats.size())
            mat = 0;
        auto& M = mats[mat];
        uint32_t pipe = PipelineFromFlags(M.flags);
        bool nocull = IsNoCullFromFlags(M.flags);
        buckets[keyToU64({I.primIndex, mat, (uint8_t)pipe, (uint8_t)(nocull ? 1 : 0)})].push_back(iid);
    }
    auto emit = [&](uint64_t key, const std::vector<uint32_t>& ids) {
        uint32_t pipe = (uint32_t)((key >> 56) & 0xFF);
        bool nocull = ((key >> 55) & 1) != 0;
        uint32_t mat = (uint32_t)((key >> 28) & ((1u << 27) - 1));
        uint32_t prim = (uint32_t)(key & ((1u << 28) - 1));
        if (prim >= prims.size() || mat >= mats.size())
            return;
        const auto& pr = prims[prim];
        const auto& mr = mats[mat];
        DrawItem di{};
        di.primIndex = prim;
        di.firstIndex = (uint32_t)(pr.indexByteOffset / sizeof(uint32_t));
        di.indexCount = pr.indexCount;
        di.materialIndex = mat;
        di.sortKey = MakeMaterialSortKey(pipe, mr, mat);
        auto push = [&](std::vector<DrawItem>& items, std::vector<uint32_t>& stream) {
            di.instanceBegin = (uint32_t)stream.size();
            di.instanceCount = (uint32_t)ids.size();
            items.push_back(di);
            stream.insert(stream.end(), ids.begin(), ids.end());
        };
        switch (pipe)
        {
        case PIPELINE_OPAQUE:
            (nocull ? push(outDL.itemsOpaqueNoCull, outDL.instOpaqueNoCull) : push(outDL.itemsOpaqueCull, outDL.instOpaqueCull));
            break;
        case PIPELINE_MASKED:
            (nocull ? push(outDL.itemsMaskedNoCull, outDL.instMaskedNoCull) : push(outDL.itemsMaskedCull, outDL.instMaskedCull));
            break;
        case PIPELINE_BLENDED:
            (nocull ? push(outDL.itemsBlendedNoCull, outDL.instBlendedNoCull) : push(outDL.itemsBlendedCull, outDL.instBlendedCull));
            break;
        }
    };
    for (auto& it : buckets)
        emit(it.first, it.second);
    auto sorter = [](const DrawItem& a, const DrawItem& b) {
        if (a.sortKey != b.sortKey)
            return a.sortKey < b.sortKey;
        if (a.materialIndex != b.materialIndex)
            return a.materialIndex < b.materialIndex;
        return a.primIndex < b.primIndex;
    };
    std::sort(outDL.itemsOpaqueCull.begin(), outDL.itemsOpaqueCull.end(), sorter);
    std::sort(outDL.itemsOpaqueNoCull.begin(), outDL.itemsOpaqueNoCull.end(), sorter);
    std::sort(outDL.itemsMaskedCull.begin(), outDL.itemsMaskedCull.end(), sorter);
    std::sort(outDL.itemsMaskedNoCull.begin(), outDL.itemsMaskedNoCull.end(), sorter);
    std::sort(outDL.itemsBlendedCull.begin(), outDL.itemsBlendedCull.end(), sorter);
    std::sort(outDL.itemsBlendedNoCull.begin(), outDL.itemsBlendedNoCull.end(), sorter);
}

// ---------------------- Pack builder ----------------------
static bool ProcessAllMeshesAndWritePack(const fs::path& outPackPath, const fs::path& glbPath, const tinygltf::Model& model, bool fastCompress)
{
    struct PrimJob
    {
        int meshIdx, primIdx;
        size_t outIdx;
    };

    size_t totalPrim = 0;
    for (int mi = 0; mi < (int)model.meshes.size(); ++mi)
        totalPrim += model.meshes[size_t(mi)].primitives.size();
    std::vector<PrimJob> jobs;
    jobs.reserve(totalPrim);
    for (int mi = 0; mi < (int)model.meshes.size(); ++mi)
    {
        const auto& mesh = model.meshes[size_t(mi)];
        for (int pi = 0; pi < (int)mesh.primitives.size(); ++pi)
            jobs.push_back({mi, pi, jobs.size()});
    }

    std::vector<BuiltPrimitive> built(jobs.size());
    std::atomic_size_t done{0};
    std::mutex logMtx;
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](const PrimJob& j) {
        BuildOnePrimitive(model, j.meshIdx, j.primIdx, built[j.outIdx]);
        size_t d = done.fetch_add(1) + 1;
        std::lock_guard<std::mutex> _g(logMtx);
        const auto& bp = built[j.outIdx];
        std::printf("[prim][%zu/%zu] mesh=%u prim=%u  v=%zu i=%zu m=%zu\n", d, jobs.size(), bp.meshIndex, bp.primIndex, bp.vertices.size(), bp.indices.size(), bp.meshlets.size());
    });

    // Flatten
    std::vector<PrimRecord> prims;
    std::vector<Vertex> blobVertices;
    std::vector<uint32_t> blobIndices;
    std::vector<IskurMeshlet> blobMeshlets;
    std::vector<uint32_t> blobMLVerts;
    std::vector<uint8_t> blobMLTris;
    std::vector<MeshletBounds> blobMLBounds;
    prims.reserve(built.size());
    auto sum = [&](auto get) {
        size_t s = 0;
        for (auto& b : built)
            s += get(b);
        return s;
    };
    blobVertices.reserve(sum([](const BuiltPrimitive& b) { return b.vertices.size(); }));
    blobIndices.reserve(sum([](const BuiltPrimitive& b) { return b.indices.size(); }));
    blobMeshlets.reserve(sum([](const BuiltPrimitive& b) { return b.meshlets.size(); }));
    blobMLVerts.reserve(sum([](const BuiltPrimitive& b) { return b.mlVerts.size(); }));
    blobMLTris.reserve(sum([](const BuiltPrimitive& b) { return b.mlTris.size(); }));
    blobMLBounds.reserve(sum([](const BuiltPrimitive& b) { return b.mlBounds.size(); }));

    for (const auto& b : built)
    {
        PrimRecord r{};
        r.meshIndex = b.meshIndex;
        r.primIndex = b.primIndex;
        r.materialIndex = b.materialIndex;
        r.vertexCount = (uint32_t)b.vertices.size();
        r.indexCount = (uint32_t)b.indices.size();
        r.meshletCount = (uint32_t)b.meshlets.size();
        r.mlVertsCount = (uint32_t)b.mlVerts.size();
        r.mlTrisByteCount = (uint32_t)b.mlTris.size();
        r.vertexByteOffset = uint64_t(blobVertices.size()) * sizeof(Vertex);
        r.indexByteOffset = uint64_t(blobIndices.size()) * sizeof(uint32_t);
        r.meshletsByteOffset = uint64_t(blobMeshlets.size()) * sizeof(IskurMeshlet);
        r.mlVertsByteOffset = uint64_t(blobMLVerts.size()) * sizeof(uint32_t);
        r.mlTrisByteOffset = uint64_t(blobMLTris.size());
        r.mlBoundsByteOffset = uint64_t(blobMLBounds.size()) * sizeof(MeshletBounds);
        prims.push_back(r);
        blobVertices.insert(blobVertices.end(), b.vertices.begin(), b.vertices.end());
        blobIndices.insert(blobIndices.end(), b.indices.begin(), b.indices.end());
        blobMeshlets.insert(blobMeshlets.end(), b.meshlets.begin(), b.meshlets.end());
        blobMLVerts.insert(blobMLVerts.end(), b.mlVerts.begin(), b.mlVerts.end());
        blobMLTris.insert(blobMLTris.end(), b.mlTris.begin(), b.mlTris.end());
        blobMLBounds.insert(blobMLBounds.end(), b.mlBounds.begin(), b.mlBounds.end());
    }

    // Instances
    std::vector<InstanceRecord> instTable;
    BuildInstanceTable(model, prims, built, instTable);
    bool haveInstances = !instTable.empty();
    // Reorder instances into CSR by prim index so INST matches draw ID expectations and loader can skip remap
    if (haveInstances)
    {
        const uint32_t primCount = (uint32_t)prims.size();
        const uint32_t instCount = (uint32_t)instTable.size();
        std::vector<uint32_t> counts(primCount, 0);
        for (const auto& r : instTable)
        {
            if (r.primIndex < primCount)
                ++counts[r.primIndex];
        }
        std::vector<uint32_t> offsets(primCount + 1, 0);
        uint32_t run = 0;
        for (uint32_t p = 0; p < primCount; ++p)
        {
            offsets[p] = run;
            run += counts[p];
        }
        offsets[primCount] = run;
        std::vector<InstanceRecord> instCSR(instCount);
        // Stable within-prim order (sequential pass) to yield deterministic layout
        for (uint32_t i = 0; i < instCount; ++i)
        {
            const auto& src = instTable[i];
            if (src.primIndex >= primCount)
                continue;
            uint32_t& cur = offsets[src.primIndex];
            instCSR[cur++] = src;
        }
        // Restore offsets prefix (optional, if needed later)
        run = 0;
        for (uint32_t p = 0; p < primCount; ++p)
        {
            const uint32_t c = counts[p];
            offsets[p] = run;
            run += c;
        }
        offsets[primCount] = run;
        instTable.swap(instCSR);
    }

    // Textures
    std::vector<TextureRecord> texTable;
    std::vector<uint8_t> texBlob;
    bool haveTextures = false;
    {
        auto imgUsage = BuildImageUsageFlags(model);
        bool ok = BuildTexturesToMemory(glbPath, model, imgUsage, fastCompress, texTable, texBlob);
        if (!ok && !model.textures.empty())
        {
            std::fprintf(stderr, "ERROR: texture embedding failed.\n");
            return false;
        }
        haveTextures = !texTable.empty();
    }

    // Samplers & materials
    std::vector<SamplerDisk> sampTable;
    std::vector<uint32_t> gltfSamplerToSamp;
    std::vector<MaterialRecord> matTable;
    std::vector<int> txhdByImage;
    BuildSamplerTable(model, sampTable, gltfSamplerToSamp);
    BuildMaterialTable(model, texTable, matTable, sampTable, gltfSamplerToSamp, txhdByImage);
    bool haveSamplers = !sampTable.empty(), haveMaterials = !matTable.empty();

    // Draw lists
    DrawListsBuilt dl;
    if (haveInstances && haveMaterials)
        BuildDrawLists(prims, instTable, matTable, dl);
    bool haveDL0C = !dl.itemsOpaqueCull.empty(), haveDL1C = !dl.itemsMaskedCull.empty(), haveDL2C = !dl.itemsBlendedCull.empty();
    bool haveDL0N = !dl.itemsOpaqueNoCull.empty(), haveDL1N = !dl.itemsMaskedNoCull.empty(), haveDL2N = !dl.itemsBlendedNoCull.empty();

    // Layout
    std::vector<ChunkRecord> chunks;
    uint32_t chunkCount = 7u + (haveTextures ? 2u : 0u) + (haveSamplers ? 1u : 0u) + (haveMaterials ? 1u : 0u) + (haveInstances ? 1u : 0u) + (haveDL0C ? 2u : 0u) + (haveDL1C ? 2u : 0u) +
                          (haveDL2C ? 2u : 0u) + (haveDL0N ? 2u : 0u) + (haveDL1N ? 2u : 0u) + (haveDL2N ? 2u : 0u);

    const uint64_t ofsHeader = 0, ofsChunkTbl = ofsHeader + sizeof(PackHeader), ofsPrimTbl = ofsChunkTbl + uint64_t(chunkCount) * sizeof(ChunkRecord);
    const uint64_t ofsVertices = ofsPrimTbl + uint64_t(prims.size()) * sizeof(PrimRecord);
    const uint64_t ofsIndices = ofsVertices + uint64_t(blobVertices.size()) * sizeof(Vertex);
    const uint64_t ofsMeshlets = ofsIndices + uint64_t(blobIndices.size()) * sizeof(uint32_t);
    const uint64_t ofsMLVerts = ofsMeshlets + uint64_t(blobMeshlets.size()) * sizeof(IskurMeshlet);
    const uint64_t ofsMLTris = ofsMLVerts + uint64_t(blobMLVerts.size()) * sizeof(uint32_t);
    const uint64_t ofsMLBounds = ofsMLTris + uint64_t(blobMLTris.size());
    uint64_t cur = ofsMLBounds + uint64_t(blobMLBounds.size()) * sizeof(MeshletBounds);

    const uint64_t ofsTxHd = haveTextures ? cur : 0;
    if (haveTextures)
        cur += uint64_t(texTable.size()) * sizeof(TextureRecord);
    const uint64_t ofsTxTb = haveTextures ? cur : 0;
    if (haveTextures)
        cur += uint64_t(texBlob.size());
    const uint64_t ofsSamp = haveSamplers ? cur : 0;
    if (haveSamplers)
        cur += uint64_t(sampTable.size()) * sizeof(SamplerDisk);
    const uint64_t ofsMatl = haveMaterials ? cur : 0;
    if (haveMaterials)
        cur += uint64_t(matTable.size()) * sizeof(MaterialRecord);
    const uint64_t ofsInst = haveInstances ? cur : 0;
    if (haveInstances)
        cur += uint64_t(instTable.size()) * sizeof(InstanceRecord);

    const uint64_t ofsDRL0 = haveDL0C ? cur : 0;
    if (haveDL0C)
        cur += uint64_t(dl.itemsOpaqueCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDRI0 = haveDL0C ? cur : 0;
    if (haveDL0C)
        cur += uint64_t(dl.instOpaqueCull.size()) * sizeof(uint32_t);
    const uint64_t ofsDRL1 = haveDL1C ? cur : 0;
    if (haveDL1C)
        cur += uint64_t(dl.itemsMaskedCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDRI1 = haveDL1C ? cur : 0;
    if (haveDL1C)
        cur += uint64_t(dl.instMaskedCull.size()) * sizeof(uint32_t);
    const uint64_t ofsDRL2 = haveDL2C ? cur : 0;
    if (haveDL2C)
        cur += uint64_t(dl.itemsBlendedCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDRI2 = haveDL2C ? cur : 0;
    if (haveDL2C)
        cur += uint64_t(dl.instBlendedCull.size()) * sizeof(uint32_t);

    const uint64_t ofsDNL0 = haveDL0N ? cur : 0;
    if (haveDL0N)
        cur += uint64_t(dl.itemsOpaqueNoCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDNI0 = haveDL0N ? cur : 0;
    if (haveDL0N)
        cur += uint64_t(dl.instOpaqueNoCull.size()) * sizeof(uint32_t);
    const uint64_t ofsDNL1 = haveDL1N ? cur : 0;
    if (haveDL1N)
        cur += uint64_t(dl.itemsMaskedNoCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDNI1 = haveDL1N ? cur : 0;
    if (haveDL1N)
        cur += uint64_t(dl.instMaskedNoCull.size()) * sizeof(uint32_t);
    const uint64_t ofsDNL2 = haveDL2N ? cur : 0;
    if (haveDL2N)
        cur += uint64_t(dl.itemsBlendedNoCull.size()) * sizeof(DrawItem);
    const uint64_t ofsDNI2 = haveDL2N ? cur : 0;
    if (haveDL2N)
        cur += uint64_t(dl.instBlendedNoCull.size()) * sizeof(uint32_t);

    auto addChunk = [&](uint32_t id, uint64_t off, uint64_t size, const void* ptr) {
        ChunkRecord r{};
        r.id = id;
        r.offset = off;
        r.size = size;
        chunks.push_back(r);
    };

    addChunk(CH_PRIM, ofsPrimTbl, uint64_t(prims.size()) * sizeof(PrimRecord), prims.data());
    addChunk(CH_VERT, ofsVertices, uint64_t(blobVertices.size()) * sizeof(Vertex), blobVertices.data());
    addChunk(CH_INDX, ofsIndices, uint64_t(blobIndices.size()) * sizeof(uint32_t), blobIndices.data());
    addChunk(CH_MSHL, ofsMeshlets, uint64_t(blobMeshlets.size()) * sizeof(IskurMeshlet), blobMeshlets.data());
    addChunk(CH_MLVT, ofsMLVerts, uint64_t(blobMLVerts.size()) * sizeof(uint32_t), blobMLVerts.data());
    addChunk(CH_MLTR, ofsMLTris, uint64_t(blobMLTris.size()), blobMLTris.data());
    addChunk(CH_MLBD, ofsMLBounds, uint64_t(blobMLBounds.size()) * sizeof(MeshletBounds), blobMLBounds.data());

    if (haveTextures)
    {
        addChunk(CH_TXHD, ofsTxHd, uint64_t(texTable.size()) * sizeof(TextureRecord), texTable.data());
        addChunk(CH_TXTB, ofsTxTb, uint64_t(texBlob.size()), texBlob.data());
    }
    if (haveSamplers)
        addChunk(CH_SAMP, ofsSamp, uint64_t(sampTable.size()) * sizeof(SamplerDisk), sampTable.data());
    if (haveMaterials)
        addChunk(CH_MATL, ofsMatl, uint64_t(matTable.size()) * sizeof(MaterialRecord), matTable.data());
    if (haveInstances)
        addChunk(CH_INST, ofsInst, uint64_t(instTable.size()) * sizeof(InstanceRecord), instTable.data());

    if (haveDL0C)
    {
        addChunk(CH_DRL0, ofsDRL0, uint64_t(dl.itemsOpaqueCull.size()) * sizeof(DrawItem), dl.itemsOpaqueCull.data());
        addChunk(CH_DRI0, ofsDRI0, uint64_t(dl.instOpaqueCull.size()) * sizeof(uint32_t), dl.instOpaqueCull.data());
    }
    if (haveDL1C)
    {
        addChunk(CH_DRL1, ofsDRL1, uint64_t(dl.itemsMaskedCull.size()) * sizeof(DrawItem), dl.itemsMaskedCull.data());
        addChunk(CH_DRI1, ofsDRI1, uint64_t(dl.instMaskedCull.size()) * sizeof(uint32_t), dl.instMaskedCull.data());
    }
    if (haveDL2C)
    {
        addChunk(CH_DRL2, ofsDRL2, uint64_t(dl.itemsBlendedCull.size()) * sizeof(DrawItem), dl.itemsBlendedCull.data());
        addChunk(CH_DRI2, ofsDRI2, uint64_t(dl.instBlendedCull.size()) * sizeof(uint32_t), dl.instBlendedCull.data());
    }

    if (haveDL0N)
    {
        addChunk(CH_DNL0, ofsDNL0, uint64_t(dl.itemsOpaqueNoCull.size()) * sizeof(DrawItem), dl.itemsOpaqueNoCull.data());
        addChunk(CH_DNI0, ofsDNI0, uint64_t(dl.instOpaqueNoCull.size()) * sizeof(uint32_t), dl.instOpaqueNoCull.data());
    }
    if (haveDL1N)
    {
        addChunk(CH_DNL1, ofsDNL1, uint64_t(dl.itemsMaskedNoCull.size()) * sizeof(DrawItem), dl.itemsMaskedNoCull.data());
        addChunk(CH_DNI1, ofsDNI1, uint64_t(dl.instMaskedNoCull.size()) * sizeof(uint32_t), dl.instMaskedNoCull.data());
    }
    if (haveDL2N)
    {
        addChunk(CH_DNL2, ofsDNL2, uint64_t(dl.itemsBlendedNoCull.size()) * sizeof(DrawItem), dl.itemsBlendedNoCull.data());
        addChunk(CH_DNI2, ofsDNI2, uint64_t(dl.instBlendedNoCull.size()) * sizeof(uint32_t), dl.instBlendedNoCull.data());
    }

    // Header + write
    PackHeader hdr{};
    std::memcpy(hdr.magic, "ISKURPK", 7);
    hdr.version = 5;
    hdr.primCount = (uint32_t)prims.size();
    hdr.chunkCount = (uint32_t)chunks.size();
    hdr.chunkTableOffset = ofsChunkTbl;
    hdr.primTableOffset = ofsPrimTbl;
    hdr.verticesOffset = ofsVertices;
    hdr.indicesOffset = ofsIndices;
    hdr.meshletsOffset = ofsMeshlets;
    hdr.mlVertsOffset = ofsMLVerts;
    hdr.mlTrisOffset = ofsMLTris;
    hdr.mlBoundsOffset = ofsMLBounds;

    std::ofstream out(outPackPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "Failed to open pack: %s\n", outPackPath.string().c_str());
        return false;
    }
    out.write((const char*)&hdr, sizeof(hdr));
    out.write((const char*)chunks.data(), chunks.size() * sizeof(ChunkRecord));
    out.write((const char*)prims.data(), prims.size() * sizeof(PrimRecord));
    out.write((const char*)blobVertices.data(), blobVertices.size() * sizeof(Vertex));
    out.write((const char*)blobIndices.data(), blobIndices.size() * sizeof(uint32_t));
    out.write((const char*)blobMeshlets.data(), blobMeshlets.size() * sizeof(IskurMeshlet));
    out.write((const char*)blobMLVerts.data(), blobMLVerts.size() * sizeof(uint32_t));
    out.write((const char*)blobMLTris.data(), blobMLTris.size());
    out.write((const char*)blobMLBounds.data(), blobMLBounds.size() * sizeof(MeshletBounds));
    if (haveTextures)
    {
        out.write((const char*)texTable.data(), texTable.size() * sizeof(TextureRecord));
        out.write((const char*)texBlob.data(), texBlob.size());
    }
    if (haveSamplers)
        out.write((const char*)sampTable.data(), sampTable.size() * sizeof(SamplerDisk));
    if (haveMaterials)
        out.write((const char*)matTable.data(), matTable.size() * sizeof(MaterialRecord));
    if (haveInstances)
        out.write((const char*)instTable.data(), instTable.size() * sizeof(InstanceRecord));
    if (haveDL0C)
    {
        out.write((const char*)dl.itemsOpaqueCull.data(), dl.itemsOpaqueCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instOpaqueCull.data(), dl.instOpaqueCull.size() * sizeof(uint32_t));
    }
    if (haveDL1C)
    {
        out.write((const char*)dl.itemsMaskedCull.data(), dl.itemsMaskedCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instMaskedCull.data(), dl.instMaskedCull.size() * sizeof(uint32_t));
    }
    if (haveDL2C)
    {
        out.write((const char*)dl.itemsBlendedCull.data(), dl.itemsBlendedCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instBlendedCull.data(), dl.instBlendedCull.size() * sizeof(uint32_t));
    }
    if (haveDL0N)
    {
        out.write((const char*)dl.itemsOpaqueNoCull.data(), dl.itemsOpaqueNoCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instOpaqueNoCull.data(), dl.instOpaqueNoCull.size() * sizeof(uint32_t));
    }
    if (haveDL1N)
    {
        out.write((const char*)dl.itemsMaskedNoCull.data(), dl.itemsMaskedNoCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instMaskedNoCull.data(), dl.instMaskedNoCull.size() * sizeof(uint32_t));
    }
    if (haveDL2N)
    {
        out.write((const char*)dl.itemsBlendedNoCull.data(), dl.itemsBlendedNoCull.size() * sizeof(DrawItem));
        out.write((const char*)dl.instBlendedNoCull.data(), dl.instBlendedNoCull.size() * sizeof(uint32_t));
    }

    if (!out.good())
    {
        std::fprintf(stderr, "Write error: %s\n", outPackPath.string().c_str());
        return false;
    }

    auto kb = [](uint64_t b) { return double(b) / 1024.0; };
    std::printf("Meshes pack written (v%u): %s\n", hdr.version, outPackPath.string().c_str());
    std::printf("  prims=%zu, verts=%zu, inds=%zu, meshlets=%zu, mlVerts=%zu, mlTris=%zu bytes, mlBounds=%zu\n", prims.size(), blobVertices.size(), blobIndices.size(), blobMeshlets.size(),
                blobMLVerts.size(), blobMLTris.size(), blobMLBounds.size());
    if (haveTextures)
    {
        uint64_t totalDDS = 0;
        for (const auto& t : texTable)
            totalDDS += t.byteSize;
        std::printf("  textures: %zu  totalDDS=%.1f KiB\n", texTable.size(), kb(totalDDS));
    }
    if (haveSamplers)
        std::printf("  samplers: %zu\n", sampTable.size());
    if (haveMaterials)
        std::printf("  materials: %zu\n", matTable.size());
    if (haveInstances)
        std::printf("  instances: %zu\n", instTable.size());
    if (haveDL0C)
        std::printf("  DL opaque (culled): items=%zu, instIDs=%zu\n", dl.itemsOpaqueCull.size(), dl.instOpaqueCull.size());
    if (haveDL1C)
        std::printf("  DL masked (culled): items=%zu, instIDs=%zu\n", dl.itemsMaskedCull.size(), dl.instMaskedCull.size());
    if (haveDL2C)
        std::printf("  DL blended (culled): items=%zu, instIDs=%zu\n", dl.itemsBlendedCull.size(), dl.instBlendedCull.size());
    if (haveDL0N)
        std::printf("  DL opaque (no-cull): items=%zu, instIDs=%zu\n", dl.itemsOpaqueNoCull.size(), dl.instOpaqueNoCull.size());
    if (haveDL1N)
        std::printf("  DL masked (no-cull): items=%zu, instIDs=%zu\n", dl.itemsMaskedNoCull.size(), dl.instMaskedNoCull.size());
    if (haveDL2N)
        std::printf("  DL blended (no-cull): items=%zu, instIDs=%zu\n", dl.itemsBlendedNoCull.size(), dl.instBlendedNoCull.size());
    return true;
}

// ---------------------- CLI ----------------------
static void PrintUsage()
{
    std::puts("IskurScenePacker\nUsage:\n  IskurScenePacker --input <scene> [--fast]\n  IskurScenePacker --all [--fast]");
}
static int WriteIskurScene(const fs::path& inGlb, const fs::path& outPack, const RunOptions& opt)
{
    if (!IsGlbFile(inGlb))
    {
        std::fprintf(stderr, "Not a GLB: %s\n", inGlb.string().c_str());
        return 3;
    }
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string warn, err;
    bool ok = loader.LoadBinaryFromFile(&model, &warn, &err, inGlb.string());
    if (!warn.empty())
        std::fprintf(stderr, "[tinygltf] warn: %s\n", warn.c_str());
    if (!ok)
    {
        if (!err.empty())
            std::fprintf(stderr, "[tinygltf] err: %s\n", err.c_str());
        return 3;
    }
    std::fprintf(stdout, "Loaded GLB: %s\n", inGlb.string().c_str());
    return ProcessAllMeshesAndWritePack(outPack, inGlb, model, opt.fast) ? 0 : 4;
}

int main(int argc, char** argv)
{
    std::chrono::steady_clock::time_point t0(std::chrono::steady_clock::now());

    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needCoUninit = SUCCEEDED(cohr);
    if (cohr == RPC_E_CHANGED_MODE)
        needCoUninit = false;
    else if (FAILED(cohr))
    {
        std::fprintf(stderr, "COM init failed: 0x%08X\n", (unsigned)cohr);
        return 1;
    }

    fs::path inSceneName;
    bool processAll = false;
    RunOptions opt;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if ((a == "-i" || a == "--input") && i + 1 < argc)
            inSceneName = argv[++i];
        else if (a == "--fast")
            opt.fast = true;
        else if (a == "--all" || a == "--all-scenes")
            processAll = true;
        else
        {
            std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            PrintUsage();
            if (needCoUninit)
                CoUninitialize();
            return 2;
        }
    }

    if (processAll)
    {
        const fs::path srcRoot = fs::path("data") / "scenes_sources", outRoot = fs::path("data") / "scenes";
        if (!(fs::exists(srcRoot) && fs::is_directory(srcRoot)))
        {
            std::fprintf(stderr, "No sources dir: %s\n", srcRoot.string().c_str());
            if (needCoUninit)
                CoUninitialize();
            return 1;
        }
        if (!fs::exists(outRoot))
        {
            std::error_code ec;
            fs::create_directories(outRoot, ec);
            if (ec)
            {
                std::fprintf(stderr, "Failed to create output dir: %s\n", outRoot.string().c_str());
                if (needCoUninit)
                    CoUninitialize();
                return 1;
            }
        }
        int total = 0, okc = 0, failed = 0, skipped = 0;
        for (const auto& de : fs::directory_iterator(srcRoot))
        {
            if (!de.is_regular_file() || de.path().extension() != ".glb")
                continue;
            std::string stem = de.path().stem().string();
            fs::path glb = de.path();
            fs::path pack = outRoot / (stem + ".iskurpack");
            if (!IsGlbFile(glb))
            {
                std::fprintf(stdout, "[skip] %s (not GLB)\n", stem.c_str());
                ++skipped;
                continue;
            }
            std::fprintf(stdout, "=== %s ===\n", stem.c_str());
            ++total;
            int rc = WriteIskurScene(glb, pack, opt);
            if (rc == 0)
                ++okc;
            else
                ++failed;
        }
        std::fprintf(stdout, "All-scenes: total=%d, ok=%d, failed=%d, skipped=%d (fast=%s)\n", total, okc, failed, skipped, opt.fast ? "yes" : "no");
        if (needCoUninit)
            CoUninitialize();
        return (failed == 0) ? 0 : 4;
    }

    if (inSceneName.empty())
    {
        PrintUsage();
        if (needCoUninit)
            CoUninitialize();
        return 1;
    }
    const std::string name = inSceneName.string();
    const fs::path glbPath = fs::path("data") / "scenes_sources" / (name + ".glb");
    const fs::path outPath = fs::path("data") / "scenes" / (name + ".iskurpack");

    if (!(fs::exists(glbPath) && fs::is_regular_file(glbPath)))
    {
        std::fprintf(stderr, "GLB not found: %s\n", glbPath.string().c_str());
        if (needCoUninit)
            CoUninitialize();
        return 1;
    }
    if (!IsGlbFile(glbPath))
    {
        std::fprintf(stderr, "Input is not GLB: %s\n", glbPath.string().c_str());
        if (needCoUninit)
            CoUninitialize();
        return 1;
    }
    {
        const auto outDir = outPath.parent_path();
        if (!fs::exists(outDir))
        {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            if (ec)
            {
                std::fprintf(stderr, "Failed to create output dir: %s\n", outDir.string().c_str());
                if (needCoUninit)
                    CoUninitialize();
                return 1;
            }
        }
    }

    int rc = WriteIskurScene(glbPath, outPath, opt);
    if (needCoUninit)
        CoUninitialize();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Total time: %.3f s\n", sec);

    return rc;
}
