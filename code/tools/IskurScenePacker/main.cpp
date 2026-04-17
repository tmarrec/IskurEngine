// Iskur Engine - Scene Packer
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "common/IskurPackFormat.h"
#include "common/Asserts.h"
#include "common/StringUtils.h"
#include "shaders/CPUGPU.h"
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXTex.h>
#include <Objbase.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <d3d12.h>
#include <fastgltf/base64.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <filesystem>
#include <fstream>
#include <meshoptimizer.h>
#include <mikktspace.h>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace DirectX;
namespace fs = std::filesystem;

[[noreturn]] static void Fatal(const char* msg)
{
    std::println("Error: {}", msg);
    std::exit(EXIT_FAILURE);
}

static void Require(bool cond, const char* msg)
{
    if (!cond)
        Fatal(msg);
}

static bool IsFiniteF32(f32 v)
{
    return std::isfinite(v);
}

static bool IsFiniteFloat2(const XMFLOAT2& v)
{
    return IsFiniteF32(v.x) && IsFiniteF32(v.y);
}

static bool IsFiniteFloat3(const XMFLOAT3& v)
{
    return IsFiniteF32(v.x) && IsFiniteF32(v.y) && IsFiniteF32(v.z);
}

static bool IsFiniteFloat4(const XMFLOAT4& v)
{
    return IsFiniteF32(v.x) && IsFiniteF32(v.y) && IsFiniteF32(v.z) && IsFiniteF32(v.w);
}

static bool IsFiniteFloat4x4(const XMFLOAT4X4& m)
{
    return IsFiniteF32(m._11) && IsFiniteF32(m._12) && IsFiniteF32(m._13) && IsFiniteF32(m._14) && IsFiniteF32(m._21) && IsFiniteF32(m._22) && IsFiniteF32(m._23) &&
           IsFiniteF32(m._24) && IsFiniteF32(m._31) && IsFiniteF32(m._32) && IsFiniteF32(m._33) && IsFiniteF32(m._34) && IsFiniteF32(m._41) && IsFiniteF32(m._42) &&
           IsFiniteF32(m._43) && IsFiniteF32(m._44);
}

struct IskurMeshlet
{
    u32 vertexOffset, triangleOffset;
    u16 vertexCount, triangleCount;
};

using namespace IEPack;

static XMMATRIX RowMatFromColArray(const f32 Mc[16])
{
    return XMMatrixSet(Mc[0], Mc[1], Mc[2], Mc[3], Mc[4], Mc[5], Mc[6], Mc[7], Mc[8], Mc[9], Mc[10], Mc[11], Mc[12], Mc[13], Mc[14], Mc[15]);
}

static XMMATRIX NodeLocalMatrix_Row(const fastgltf::Node& n)
{
    const fastgltf::math::fmat4x4 m = fastgltf::getTransformMatrix(n, fastgltf::math::fmat4x4(1.0f));
    f32 Mc[16] = {
        m.col(0).x(), m.col(0).y(), m.col(0).z(), m.col(0).w(), m.col(1).x(), m.col(1).y(), m.col(1).z(), m.col(1).w(),
        m.col(2).x(), m.col(2).y(), m.col(2).z(), m.col(2).w(), m.col(3).x(), m.col(3).y(), m.col(3).z(), m.col(3).w(),
    };
    return RowMatFromColArray(Mc);
}

static f32 SignNotZeroF(f32 v)
{
    return (v >= 0.0f) ? 1.0f : -1.0f;
}

static XMFLOAT3 NormalizeSafe(const XMFLOAT3& n)
{
    const f32 len2 = n.x * n.x + n.y * n.y + n.z * n.z;
    if (len2 <= 1e-20f)
        return XMFLOAT3(0.0f, 0.0f, 1.0f);
    const f32 invLen = 1.0f / std::sqrt(len2);
    return XMFLOAT3(n.x * invLen, n.y * invLen, n.z * invLen);
}

static u32 PackNormalOctSnorm16(const XMFLOAT3& inNormal)
{
    XMFLOAT3 n = NormalizeSafe(inNormal);
    const f32 invL1 = 1.0f / (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
    f32 ex = n.x * invL1;
    f32 ey = n.y * invL1;
    if (n.z < 0.0f)
    {
        const f32 ox = (1.0f - std::abs(ey)) * SignNotZeroF(ex);
        const f32 oy = (1.0f - std::abs(ex)) * SignNotZeroF(ey);
        ex = ox;
        ey = oy;
    }

    const auto toSnorm16 = [](f32 v) -> i16 {
        v = std::clamp(v, -1.0f, 1.0f);
        int q = static_cast<int>(std::lround(v * 32767.0f));
        q = std::clamp(q, -32767, 32767);
        return static_cast<i16>(q);
    };

    const u16 qx = static_cast<u16>(toSnorm16(ex));
    const u16 qy = static_cast<u16>(toSnorm16(ey));
    return static_cast<u32>(qx) | (static_cast<u32>(qy) << 16);
}

static XMFLOAT3 UnpackNormalOctSnorm16(u32 packed)
{
    const i16 qx = static_cast<i16>(packed & 0xFFFFu);
    const i16 qy = static_cast<i16>((packed >> 16) & 0xFFFFu);

    f32 ex = static_cast<f32>(qx) / 32767.0f;
    f32 ey = static_cast<f32>(qy) / 32767.0f;
    ex = std::max(-1.0f, ex);
    ey = std::max(-1.0f, ey);

    f32 x = ex;
    f32 y = ey;
    f32 z = 1.0f - std::abs(x) - std::abs(y);
    if (z < 0.0f)
    {
        const f32 ox = (1.0f - std::abs(y)) * SignNotZeroF(x);
        const f32 oy = (1.0f - std::abs(x)) * SignNotZeroF(y);
        x = ox;
        y = oy;
    }
    return NormalizeSafe(XMFLOAT3(x, y, z));
}

static u32 PackTexCoordHalf2(const XMFLOAT2& uv)
{
    const u16 ux = DirectX::PackedVector::XMConvertFloatToHalf(uv.x);
    const u16 uy = DirectX::PackedVector::XMConvertFloatToHalf(uv.y);
    return static_cast<u32>(ux) | (static_cast<u32>(uy) << 16);
}

static XMFLOAT2 UnpackTexCoordHalf2(u32 packed)
{
    const u16 ux = static_cast<u16>(packed & 0xFFFFu);
    const u16 uy = static_cast<u16>((packed >> 16) & 0xFFFFu);
    return XMFLOAT2(DirectX::PackedVector::XMConvertHalfToFloat(ux), DirectX::PackedVector::XMConvertHalfToFloat(uy));
}

struct PackedColorRGBA16Unorm
{
    u32 lo;
    u32 hi;
};

static PackedColorRGBA16Unorm PackColorRGBA16Unorm(const XMFLOAT4& color)
{
    const auto toUnorm16 = [](f32 v) -> u32 {
        const f32 c = std::clamp(v, 0.0f, 1.0f);
        const int q = static_cast<int>(std::lround(c * 65535.0f));
        return static_cast<u32>(std::clamp(q, 0, 65535));
    };

    const u32 r = toUnorm16(color.x);
    const u32 g = toUnorm16(color.y);
    const u32 b = toUnorm16(color.z);
    const u32 a = toUnorm16(color.w);
    return PackedColorRGBA16Unorm{r | (g << 16), b | (a << 16)};
}

static u32 PackTangentR10G10B10A2(const XMFLOAT3& inTangent, f32 handedness)
{
    XMFLOAT3 t = inTangent;
    const f32 len2 = t.x * t.x + t.y * t.y + t.z * t.z;
    if (len2 <= 1e-20f)
    {
        t = XMFLOAT3(1.0f, 0.0f, 0.0f);
    }
    else
    {
        const f32 invLen = 1.0f / std::sqrt(len2);
        t.x *= invLen;
        t.y *= invLen;
        t.z *= invLen;
    }

    const auto toUnorm10 = [](f32 v) -> u32 {
        const f32 u = std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f);
        const int q = static_cast<int>(std::lround(u * 1023.0f));
        return static_cast<u32>(std::clamp(q, 0, 1023));
    };

    const u32 x = toUnorm10(t.x);
    const u32 y = toUnorm10(t.y);
    const u32 z = toUnorm10(t.z);
    const u32 a2 = (handedness < 0.0f) ? 0u : 3u;
    return x | (y << 10) | (z << 20) | (a2 << 30);
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

static std::string SceneStemFromArg(const std::string& sceneArg)
{
    const std::string lower = ToLowerAscii(sceneArg);

    if (sceneArg.size() >= 4)
    {
        if (lower.ends_with(".glb"))
            return sceneArg.substr(0, sceneArg.size() - 4);
    }
    constexpr size_t packExtLen = sizeof(PACK_FILE_EXTENSION) - 1;
    if (sceneArg.size() >= packExtLen && lower.ends_with(PACK_FILE_EXTENSION))
        return sceneArg.substr(0, sceneArg.size() - packExtLen);
    return sceneArg;
}

static bool ResolveSceneSourcePath(const fs::path& srcRoot, const std::string& sceneArg, fs::path& outGlbPath)
{
    const std::string stem = SceneStemFromArg(sceneArg);
    const fs::path direct = srcRoot / (stem + ".glb");
    if (fs::exists(direct) && fs::is_regular_file(direct) && IsGlbFile(direct))
    {
        outGlbPath = direct;
        return true;
    }

    if (!fs::exists(srcRoot) || !fs::is_directory(srcRoot))
        return false;

    const std::string stemLower = ToLowerAscii(stem);
    for (const auto& de : fs::directory_iterator(srcRoot))
    {
        if (!de.is_regular_file())
            continue;

        const fs::path p = de.path();
        if (!EqualsIgnoreCaseAscii(p.extension().string(), ".glb"))
            continue;

        if (!EqualsIgnoreCaseAscii(p.stem().string(), stemLower))
            continue;

        if (!IsGlbFile(p))
            continue;

        outGlbPath = p;
        return true;
    }

    return false;
}

static fs::path FindRepoRoot(const fs::path& exePath)
{
#ifdef ISKUR_ROOT
    fs::path root = fs::path(ISKUR_ROOT);
    if (!root.empty() && fs::exists(root / "data"))
        return root;
#endif

    fs::path cwd = fs::current_path();
    if (fs::exists(cwd / "data"))
        return cwd;

    fs::path cur = exePath;
    if (cur.has_filename())
        cur = cur.parent_path();

    for (int i = 0; i < 5 && !cur.empty(); ++i)
    {
        if (fs::exists(cur / "data"))
            return cur;
        cur = cur.parent_path();
    }

    return cwd;
}

static HRESULT LoadAnyImageMemory(const u8* bytes, size_t size, ScratchImage& out, WIC_FLAGS wicFlags)
{
    if (size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
        return LoadFromDDSMemory(bytes, size, DDS_FLAGS_NONE, nullptr, out);
    if (size >= 10 && std::memcmp(bytes, "#?RADIANCE", 10) == 0)
        return LoadFromHDRMemory(bytes, size, nullptr, out);
    return LoadFromWICMemory(bytes, size, wicFlags, nullptr, out);
}

enum : u32
{
    IMG_BASECOLOR = 1u << 0,
    IMG_NORMAL = 1u << 1,
    IMG_METALROUGH = 1u << 2,
    IMG_OCCLUSION = 1u << 3,
    IMG_EMISSIVE = 1u << 4
};

static std::vector<u32> BuildImageUsageFlags(const fastgltf::Asset& asset)
{
    std::vector<u32> flags(asset.images.size(), 0);
    auto mark = [&](const auto& opt, u32 f) {
        if (!opt)
            return;
        const size_t ti = opt->textureIndex;
        if (ti >= asset.textures.size())
            return;
        const auto& tex = asset.textures[ti];
        if (!tex.imageIndex)
            return;
        const size_t img = *tex.imageIndex;
        if (img >= flags.size())
            return;
        flags[img] |= f;
    };
    for (const auto& m : asset.materials)
    {
        mark(m.pbrData.baseColorTexture, IMG_BASECOLOR);
        mark(m.normalTexture, IMG_NORMAL);
        mark(m.pbrData.metallicRoughnessTexture, IMG_METALROUGH);
        mark(m.occlusionTexture, IMG_OCCLUSION);
        mark(m.emissiveTexture, IMG_EMISSIVE);
    }
    return flags;
}

constexpr int kOpacityMicromapStates = 4;
constexpr int kOpacityMicromapMaxLevel = 6;
constexpr f32 kOpacityMicromapTargetEdge = 3.0f;

struct DecodedRgba8Image
{
    u32 width = 0;
    u32 height = 0;
    u32 rowPitch = 0;
    std::vector<u8> pixels;
};

struct MaskMaterialAlphaSource
{
    bool enabled = false;
    u32 imageIndex = UINT32_MAX;
    u32 width = 0;
    u32 height = 0;
    u32 rowPitch = 0;
    std::vector<u8> pixels;
};

struct OMMBuildStats
{
    u32 maskedPrimitiveCount = 0;
    u64 entryCount = 0;
    u64 dataBytesBeforeCompaction = 0;
    u64 dataBytesAfterCompaction = 0;
};

// ---- MikkTSpace ----
static int mk_getNumFaces(const SMikkTSpaceContext* ctx);
static int mk_getNumVerticesOfFace(const SMikkTSpaceContext*, const int)
{
    return 3;
}
static void mk_getPosition(const SMikkTSpaceContext* ctx, f32 out[3], const int f, const int v);
static void mk_getNormal(const SMikkTSpaceContext* ctx, f32 out[3], const int f, const int v);
static void mk_getTexCoord(const SMikkTSpaceContext* ctx, f32 out[2], const int f, const int v);
static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const f32 t[3], const f32 sign, const int f, const int v);

struct MikkUserData
{
    const std::vector<u32>* indices;
    std::vector<Vertex>* verts;
};

static void ComputeTangentsMikk(const std::vector<u32>& idx, std::vector<Vertex>& verts)
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

static void mk_getPosition(const SMikkTSpaceContext* ctx, f32 out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    u32 i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    auto& P = (*ud->verts)[i].position;
    out[0] = P.x;
    out[1] = P.y;
    out[2] = P.z;
}

static void mk_getNormal(const SMikkTSpaceContext* ctx, f32 out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    u32 i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    XMFLOAT3 N = UnpackNormalOctSnorm16((*ud->verts)[i].normalPacked);
    out[0] = N.x;
    out[1] = N.y;
    out[2] = N.z;
}

static void mk_getTexCoord(const SMikkTSpaceContext* ctx, f32 out[2], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    u32 i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    XMFLOAT2 uv = UnpackTexCoordHalf2((*ud->verts)[i].texCoordPacked);
    out[0] = uv.x;
    out[1] = uv.y;
}

static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const f32 t[3], const f32 s, const int f, const int v)
{
    auto* ud = static_cast<MikkUserData*>(ctx->m_pUserData);
    u32 i = (*ud->indices)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    const f32 handedness = -s;
    (*ud->verts)[i].tangentPacked = PackTangentR10G10B10A2(XMFLOAT3(t[0], t[1], t[2]), handedness);
}

static bool DecodeDataUri(const fastgltf::URIView& uri, std::vector<u8>& out)
{
    if (!uri.isDataUri())
        return false;
    const std::string_view raw = uri.string();
    const auto comma = raw.find(',');
    if (comma == std::string_view::npos)
        return false;
    const std::string_view meta = raw.substr(5, comma - 5);
    const std::string_view data = raw.substr(comma + 1);
    if (meta.find(";base64") != std::string_view::npos)
    {
        auto decoded = fastgltf::base64::decode(data);
        out.assign(decoded.begin(), decoded.end());
        return true;
    }
    std::string decoded(data);
    fastgltf::URI::decodePercents(decoded);
    out.assign(decoded.begin(), decoded.end());
    return true;
}

static bool LoadUriBytes(const fastgltf::URIView& uri, const fs::path& baseDir, std::vector<u8>& owned)
{
    if (uri.isDataUri())
        return DecodeDataUri(uri, owned);
    std::string path = std::string(uri.string());
    fastgltf::URI::decodePercents(path);
    fs::path p = baseDir / fs::u8path(path);
    std::ifstream f(p, std::ios::binary);
    if (!f.good())
        return false;
    f.seekg(0, std::ios::end);
    const size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    owned.resize(sz);
    f.read(reinterpret_cast<char*>(owned.data()), static_cast<std::streamsize>(sz));
    return f.good();
}

static bool LoadBufferViewBytes(const fastgltf::Asset& asset, const fastgltf::BufferView& view, const fs::path& baseDir, const u8*& bytes, size_t& size, std::vector<u8>& owned)
{
    if (view.bufferIndex >= asset.buffers.size())
        return false;

    const auto& buf = asset.buffers[view.bufferIndex];
    const size_t offset = view.byteOffset;
    const size_t length = view.byteLength;

    auto setView = [&](const u8* data, size_t dataSize) -> bool {
        if (offset + length > dataSize)
            return false;
        bytes = data + offset;
        size = length;
        return true;
    };

    return std::visit(fastgltf::visitor{
                          [&](const fastgltf::sources::Array& arr) -> bool { return setView(reinterpret_cast<const u8*>(arr.bytes.data()), arr.bytes.size_bytes()); },
                          [&](const fastgltf::sources::Vector& vec) -> bool { return setView(reinterpret_cast<const u8*>(vec.bytes.data()), vec.bytes.size()); },
                          [&](const fastgltf::sources::ByteView& bv) -> bool { return setView(reinterpret_cast<const u8*>(bv.bytes.data()), bv.bytes.size()); },
                          [&](const fastgltf::sources::URI& uri) -> bool {
                              fastgltf::URIView view = uri.uri;
                              if (!LoadUriBytes(view, baseDir, owned))
                                  return false;
                              return setView(owned.data(), owned.size());
                          },
                          [&](auto&) -> bool { return false; },
                      },
                      buf.data);
}

static bool LoadImageBytes(const fs::path& baseDir, const fastgltf::Asset& asset, size_t imgIndex, const u8*& bytes, size_t& size, std::vector<u8>& owned)
{
    if (imgIndex >= asset.images.size())
        return false;

    const auto& img = asset.images[imgIndex];
    return std::visit(fastgltf::visitor{
                          [&](const fastgltf::sources::URI& filePath) -> bool {
                              fastgltf::URIView view = filePath.uri;
                              if (!LoadUriBytes(view, baseDir, owned))
                                  return false;
                              bytes = owned.data();
                              size = owned.size();
                              return true;
                          },
                          [&](const fastgltf::sources::Array& arr) -> bool {
                              bytes = reinterpret_cast<const u8*>(arr.bytes.data());
                              size = arr.bytes.size_bytes();
                              return true;
                          },
                          [&](const fastgltf::sources::Vector& vec) -> bool {
                              bytes = reinterpret_cast<const u8*>(vec.bytes.data());
                              size = vec.bytes.size();
                              return true;
                          },
                          [&](const fastgltf::sources::ByteView& bv) -> bool {
                              bytes = reinterpret_cast<const u8*>(bv.bytes.data());
                              size = bv.bytes.size();
                              return true;
                          },
                          [&](const fastgltf::sources::BufferView& view) -> bool {
                              if (view.bufferViewIndex >= asset.bufferViews.size())
                                  return false;
                              return LoadBufferViewBytes(asset, asset.bufferViews[view.bufferViewIndex], baseDir, bytes, size, owned);
                          },
                          [&](auto&) -> bool { return false; },
                      },
                      img.data);
}

static DecodedRgba8Image DecodeImageToRgba8(const fs::path& glbPath, const fastgltf::Asset& asset, size_t imgIndex)
{
    const fs::path baseDir = glbPath.parent_path();
    const u8* raw = nullptr;
    size_t rawSize = 0;
    std::vector<u8> owned;
    if (!LoadImageBytes(baseDir, asset, imgIndex, raw, rawSize, owned))
        Fatal("Failed to load image bytes from glTF");

    ScratchImage loaded;
    const HRESULT hrDec = LoadAnyImageMemory(raw, rawSize, loaded, WIC_FLAGS_NONE);
    if (!IE_Try(hrDec))
        Fatal("Image decode failed");

    const DXGI_FORMAT wantBase = DXGI_FORMAT_R8G8B8A8_UNORM;
    const Image* srcImage = loaded.GetImage(0, 0, 0);
    const TexMetadata loadedMeta = loaded.GetMetadata();
    ScratchImage converted;
    if (loadedMeta.format != wantBase)
    {
        const HRESULT hrC = Convert(loaded.GetImages(), loaded.GetImageCount(), loadedMeta, wantBase, TEX_FILTER_DEFAULT, 0.0f, converted);
        if (!IE_Try(hrC))
            Fatal("Image format conversion failed");
        srcImage = converted.GetImage(0, 0, 0);
    }

    Require(srcImage != nullptr, "Decoded image has no top-level surface");
    Require(srcImage->width <= UINT32_MAX && srcImage->height <= UINT32_MAX, "Decoded image dimensions exceed pack format limits");
    Require(srcImage->rowPitch >= srcImage->width * 4, "Decoded image row pitch is too small");

    DecodedRgba8Image out{};
    out.width = static_cast<u32>(srcImage->width);
    out.height = static_cast<u32>(srcImage->height);
    out.rowPitch = out.width * 4u;
    out.pixels.resize(static_cast<size_t>(out.rowPitch) * static_cast<size_t>(out.height));
    for (u32 y = 0; y < out.height; ++y)
    {
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * out.rowPitch, srcImage->pixels + static_cast<size_t>(y) * srcImage->rowPitch, out.rowPitch);
    }

    return out;
}

static u8 QuantizeUnorm8(f32 value)
{
    const f32 clamped = std::clamp(value, 0.0f, 1.0f);
    int q = static_cast<int>(std::lround(clamped * 255.0f));
    q = std::clamp(q, 0, 255);
    return static_cast<u8>(q);
}

static f32 RemapAlphaToHalfCutoff(f32 alpha, f32 alphaCutoff)
{
    const f32 cutoff = std::clamp(alphaCutoff, 0.0f, 1.0f);
    const f32 scale = 0.5f / std::max(cutoff, 1.0f - cutoff);
    const f32 bias = 0.5f - scale * cutoff;
    return std::clamp(scale * alpha + bias, 0.0f, 1.0f);
}

static std::vector<MaskMaterialAlphaSource> BuildMaskMaterialAlphaSources(const fs::path& glbPath, const fastgltf::Asset& asset)
{
    std::vector<MaskMaterialAlphaSource> out(asset.materials.size());
    std::unordered_map<u32, DecodedRgba8Image> imageCache;

    for (size_t i = 0; i < asset.materials.size(); ++i)
    {
        const fastgltf::Material& material = asset.materials[i];
        if (material.alphaMode == fastgltf::AlphaMode::Opaque)
            continue;

        if (!material.pbrData.baseColorTexture)
        {
            std::println("Error: alpha-tested material {} has no baseColor texture for OMM generation", i);
            std::exit(EXIT_FAILURE);
        }

        const size_t textureIndex = material.pbrData.baseColorTexture->textureIndex;
        if (textureIndex >= asset.textures.size())
        {
            std::println("Error: alpha-tested material {} references invalid baseColor texture {}", i, textureIndex);
            std::exit(EXIT_FAILURE);
        }

        const auto& texture = asset.textures[textureIndex];
        if (!texture.imageIndex || *texture.imageIndex >= asset.images.size())
        {
            std::println("Error: alpha-tested material {} has no usable baseColor image for OMM generation", i);
            std::exit(EXIT_FAILURE);
        }

        const u32 imageIndex = static_cast<u32>(*texture.imageIndex);
        auto cacheIt = imageCache.find(imageIndex);
        if (cacheIt == imageCache.end())
            cacheIt = imageCache.emplace(imageIndex, DecodeImageToRgba8(glbPath, asset, imageIndex)).first;

        const f32 alphaScale = static_cast<f32>(material.pbrData.baseColorFactor.w());
        const f32 alphaCutoff = static_cast<f32>(material.alphaCutoff);
        if (!IsFiniteF32(alphaScale) || alphaScale < 0.0f)
        {
            std::println("Error: alpha-tested material {} has invalid baseColor alpha factor {}", i, alphaScale);
            std::exit(EXIT_FAILURE);
        }
        if (!IsFiniteF32(alphaCutoff))
        {
            std::println("Error: alpha-tested material {} has invalid alphaCutoff {}", i, alphaCutoff);
            std::exit(EXIT_FAILURE);
        }

        MaskMaterialAlphaSource& source = out[i];
        source.enabled = true;
        source.imageIndex = imageIndex;
        source.width = cacheIt->second.width;
        source.height = cacheIt->second.height;
        source.rowPitch = cacheIt->second.rowPitch;
        source.pixels = cacheIt->second.pixels;

        if (alphaScale != 1.0f || alphaCutoff != 0.5f)
        {
            for (size_t p = 3; p < source.pixels.size(); p += 4)
            {
                const f32 alpha = (static_cast<f32>(source.pixels[p]) / 255.0f) * alphaScale;
                source.pixels[p] = QuantizeUnorm8(RemapAlphaToHalfCutoff(alpha, alphaCutoff));
            }
        }
    }

    return out;
}

static void BuildTexturesToMemory(const fs::path& glbPath, const fastgltf::Asset& asset, const std::vector<u32>& imgUsage, bool fastCompress, std::vector<TextureRecord>& outTable,
                                  std::vector<TextureSubresourceRecord>& outSubresources, std::vector<u8>& outBlob)
{
    const fs::path baseDir = glbPath.parent_path();
    std::vector<u8> done(asset.images.size(), 0);

    for (size_t t = 0; t < asset.textures.size(); ++t)
    {
        const auto& tex = asset.textures[t];
        if (!tex.imageIndex)
            continue;

        const size_t imgIndex = *tex.imageIndex;
        if (imgIndex >= asset.images.size())
            continue;
        if (done[imgIndex])
            continue;
        const auto& img = asset.images[imgIndex];
        std::string srcDesc;
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::sources::URI& uri) { srcDesc = std::string(uri.uri.string()); },
                       [&](const fastgltf::sources::BufferView& view) { srcDesc = std::string("bufferView#") + std::to_string(view.bufferViewIndex); },
                       [&](const fastgltf::sources::Array&) { srcDesc = "embedded"; },
                       [&](const fastgltf::sources::Vector&) { srcDesc = "embedded"; },
                       [&](const fastgltf::sources::ByteView&) { srcDesc = "embedded"; },
                       [&](auto&) { srcDesc = "unknown"; },
                   },
                   img.data);

        const u8* raw = nullptr;
        size_t rawSize = 0;
        std::vector<u8> owned;

        bool okLoad = LoadImageBytes(baseDir, asset, imgIndex, raw, rawSize, owned);
        if (!okLoad)
            Fatal("Failed to load image bytes from glTF");

        std::println("[tex {}][img {}] src=\"{}\"", t, imgIndex, srcDesc);

        const u32 usage = (imgIndex < imgUsage.size()) ? imgUsage[imgIndex] : 0u;
        const bool isNormal = (usage & IMG_NORMAL) != 0;
        const bool isNonColor = isNormal || ((usage & (IMG_METALROUGH | IMG_OCCLUSION)) != 0);
        const bool isSRGB = !isNonColor && ((usage & (IMG_BASECOLOR | IMG_EMISSIVE)) != 0);
        if ((usage & (IMG_BASECOLOR | IMG_EMISSIVE)) && (usage & (IMG_NORMAL | IMG_METALROUGH | IMG_OCCLUSION)))
            std::println("Warning: image {} used as both color and non-color; treating as non-color", imgIndex);
        DXGI_FORMAT wantBase = isSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

        ScratchImage loaded;
        const WIC_FLAGS wicFlags = isNonColor ? WIC_FLAGS_IGNORE_SRGB : WIC_FLAGS_NONE;
        HRESULT hrDec = LoadAnyImageMemory(raw, rawSize, loaded, wicFlags);
        if (!IE_Try(hrDec))
            Fatal("Image decode failed");

        const Image* srcImages = nullptr;
        size_t srcCount = 0;
        TexMetadata meta{};
        ScratchImage converted;

        bool didConvert = loaded.GetMetadata().format != wantBase;
        if (didConvert)
        {
            HRESULT hrC = Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(), wantBase, TEX_FILTER_DEFAULT, 0.0f, converted);
            if (!IE_Try(hrC))
                Fatal("Image format conversion failed");
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
        TEX_FILTER_FLAGS mipFilter = TEX_FILTER_LINEAR;
        if (isSRGB)
            mipFilter = static_cast<TEX_FILTER_FLAGS>(mipFilter | TEX_FILTER_SRGB);
        if (isNonColor)
            mipFilter = static_cast<TEX_FILTER_FLAGS>(mipFilter | TEX_FILTER_FORCE_NON_WIC);

        if (SUCCEEDED(GenerateMipMaps(srcImages, srcCount, meta, mipFilter, 0, mip)))
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
        if (!IE_Try(hrComp) && (comp & TEX_COMPRESS_BC7_QUICK))
        {
            TEX_COMPRESS_FLAGS retryComp = static_cast<TEX_COMPRESS_FLAGS>(comp & ~TEX_COMPRESS_BC7_QUICK);
            hrComp = Compress(srcImages, srcCount, meta, compFmt, retryComp, 0.5f, bc);
        }

        if (!IE_Try(hrComp))
            Fatal("BC compression failed");

        const TexMetadata& bcMeta = bc.GetMetadata();
        const Image* bcImages = bc.GetImages();
        const size_t bcImageCount = bc.GetImageCount();
        Require(bcImages != nullptr && bcImageCount > 0, "Compressed texture has no subresources");
        Require(bcImageCount <= UINT32_MAX, "Texture subresource count exceeds pack format limits");
        Require(outSubresources.size() <= UINT32_MAX, "Texture subresource table exceeds pack format limits");

        TextureRecord tr{};
        tr.imageIndex = static_cast<u32>(imgIndex);
        tr.format = static_cast<u32>(bcMeta.format);
        tr.dimension = static_cast<u32>(bcMeta.dimension);
        tr.miscFlags = static_cast<u32>(bcMeta.miscFlags);
        tr.miscFlags2 = static_cast<u32>(bcMeta.miscFlags2);
        tr.width = static_cast<u32>(bcMeta.width);
        tr.height = static_cast<u32>(bcMeta.height);
        tr.depth = static_cast<u32>(bcMeta.depth);
        tr.arraySize = static_cast<u32>(bcMeta.arraySize);
        tr.mipLevels = static_cast<u32>(bcMeta.mipLevels);
        tr.subresourceOffset = static_cast<u32>(outSubresources.size());
        tr.subresourceCount = static_cast<u32>(bcImageCount);
        tr.byteOffset = static_cast<u64>(outBlob.size());

        for (size_t i = 0; i < bcImageCount; ++i)
        {
            const Image& image = bcImages[i];
            Require(image.rowPitch <= UINT32_MAX, "Texture row pitch exceeds pack format limits");
            Require(image.slicePitch <= UINT32_MAX, "Texture slice pitch exceeds pack format limits");
            TextureSubresourceRecord subresource{};
            subresource.byteOffset = static_cast<u64>(outBlob.size()) - tr.byteOffset;
            subresource.byteSize = static_cast<u64>(image.slicePitch);
            subresource.rowPitch = static_cast<u32>(image.rowPitch);
            subresource.slicePitch = static_cast<u32>(image.slicePitch);
            outSubresources.push_back(subresource);
            outBlob.insert(outBlob.end(), image.pixels, image.pixels + image.slicePitch);
        }

        tr.byteSize = static_cast<u64>(outBlob.size()) - tr.byteOffset;
        outTable.push_back(tr);
        done[imgIndex] = 1;
    }
}

static D3D12_TEXTURE_ADDRESS_MODE MapWrapModeGLTFToD3D12Addr(fastgltf::Wrap wrap)
{
    switch (wrap)
    {
    case fastgltf::Wrap::MirroredRepeat:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case fastgltf::Wrap::ClampToEdge:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

struct FilterInfo
{
    D3D12_FILTER filter;
    UINT maxAnisotropy;
};

static FilterInfo BakeD3D12Filter(fastgltf::Optional<fastgltf::Filter> minFilter, fastgltf::Optional<fastgltf::Filter> magFilter)
{
    constexpr UINT kMaterialMaxAnisotropy = 16;
    const fastgltf::Filter minF = minFilter ? *minFilter : fastgltf::Filter::LinearMipMapLinear;
    const fastgltf::Filter magF = magFilter ? *magFilter : fastgltf::Filter::Linear;

    if (minF == fastgltf::Filter::NearestMipMapNearest && magF == fastgltf::Filter::Nearest)
        return {D3D12_FILTER_MIN_MAG_MIP_POINT, 1};

    return {D3D12_FILTER_ANISOTROPIC, kMaterialMaxAnisotropy};
}

static D3D12_SAMPLER_DESC MakeDefaultSamplerDesc()
{
    D3D12_SAMPLER_DESC samplerDesc{};
    FilterInfo fi = BakeD3D12Filter(fastgltf::Filter::LinearMipMapLinear, fastgltf::Filter::Linear);
    samplerDesc.Filter = fi.filter;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    samplerDesc.MaxAnisotropy = fi.maxAnisotropy;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.BorderColor[0] = 0.0f;
    samplerDesc.BorderColor[1] = 0.0f;
    samplerDesc.BorderColor[2] = 0.0f;
    samplerDesc.BorderColor[3] = 0.0f;
    return samplerDesc;
}

static bool HasMissingSampler(const fastgltf::Asset& asset)
{
    for (const auto& t : asset.textures)
    {
        if (!t.samplerIndex || *t.samplerIndex >= asset.samplers.size())
            return true;
    }
    return false;
}

static u32 BuildSamplerTable(const fastgltf::Asset& asset, std::vector<D3D12_SAMPLER_DESC>& outSamp)
{
    for (const fastgltf::Sampler& s : asset.samplers)
    {
        D3D12_SAMPLER_DESC samplerDesc;
        FilterInfo fi = BakeD3D12Filter(s.minFilter, s.magFilter);
        samplerDesc.Filter = fi.filter;
        samplerDesc.AddressU = MapWrapModeGLTFToD3D12Addr(s.wrapS);
        samplerDesc.AddressV = MapWrapModeGLTFToD3D12Addr(s.wrapT);
        samplerDesc.AddressW = samplerDesc.AddressV;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.MaxAnisotropy = fi.maxAnisotropy;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor[0] = 0.0f;
        samplerDesc.BorderColor[1] = 0.0f;
        samplerDesc.BorderColor[2] = 0.0f;
        samplerDesc.BorderColor[3] = 0.0f;
        outSamp.push_back(samplerDesc);
    }

    u32 defaultSamplerIndex = UINT32_MAX;
    if (outSamp.empty() || HasMissingSampler(asset))
    {
        defaultSamplerIndex = static_cast<u32>(outSamp.size());
        outSamp.push_back(MakeDefaultSamplerDesc());
    }
    return defaultSamplerIndex;
}

static void BuildMaterialTable(const fastgltf::Asset& asset, const std::vector<TextureRecord>& texTable, std::vector<MaterialRecord>& outMatl, u32 defaultSamplerIndex)
{
    std::unordered_map<int, int> texToTxhd;
    texToTxhd.reserve(texTable.size());
    for (size_t i = 0; i < texTable.size(); ++i)
        texToTxhd[static_cast<int>(texTable[i].imageIndex)] = static_cast<int>(i);

    auto texIndexToTxhd = [&](size_t texIndex) -> int {
        if (texIndex >= asset.textures.size())
            return -1;
        const auto& tex = asset.textures[texIndex];
        if (!tex.imageIndex)
            return -1;
        int img = static_cast<int>(*tex.imageIndex);
        auto it = texToTxhd.find(img);
        return (it == texToTxhd.end()) ? -1 : it->second;
    };

    auto parseAlphaMode = [](const fastgltf::Material& m) -> u32 {
        if (m.alphaMode == fastgltf::AlphaMode::Mask)
            return MATF_ALPHA_MASK;
        if (m.alphaMode == fastgltf::AlphaMode::Blend)
            return MATF_ALPHA_BLEND;
        return 0;
    };

    const size_t matCount = std::max<size_t>(1, asset.materials.size());
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
        mr.flags = 0;

        if (!asset.materials.empty())
        {
            const fastgltf::Material& m = asset.materials[i];

            mr.baseColorFactor[0] = static_cast<f32>(m.pbrData.baseColorFactor.x());
            mr.baseColorFactor[1] = static_cast<f32>(m.pbrData.baseColorFactor.y());
            mr.baseColorFactor[2] = static_cast<f32>(m.pbrData.baseColorFactor.z());
            mr.baseColorFactor[3] = static_cast<f32>(m.pbrData.baseColorFactor.w());

            mr.metallicFactor = static_cast<f32>(m.pbrData.metallicFactor);
            mr.roughnessFactor = static_cast<f32>(m.pbrData.roughnessFactor);

            mr.emissiveFactor[0] = static_cast<f32>(m.emissiveFactor.x());
            mr.emissiveFactor[1] = static_cast<f32>(m.emissiveFactor.y());
            mr.emissiveFactor[2] = static_cast<f32>(m.emissiveFactor.z());

            if (m.alphaMode == fastgltf::AlphaMode::Mask)
                mr.alphaCutoff = static_cast<f32>(m.alphaCutoff);

            mr.flags |= parseAlphaMode(m);
            if (m.doubleSided)
                mr.flags |= MATF_DOUBLE_SIDED;

            Require(IsFiniteF32(mr.baseColorFactor[0]) && IsFiniteF32(mr.baseColorFactor[1]) && IsFiniteF32(mr.baseColorFactor[2]) && IsFiniteF32(mr.baseColorFactor[3]),
                    "Material baseColorFactor contains NaN/Inf");
            Require(IsFiniteF32(mr.metallicFactor) && IsFiniteF32(mr.roughnessFactor) && IsFiniteF32(mr.normalScale) && IsFiniteF32(mr.occlusionStrength) &&
                        IsFiniteF32(mr.alphaCutoff),
                    "Material scalar parameter contains NaN/Inf");
            Require(IsFiniteF32(mr.emissiveFactor[0]) && IsFiniteF32(mr.emissiveFactor[1]) && IsFiniteF32(mr.emissiveFactor[2]), "Material emissiveFactor contains NaN/Inf");

            auto chooseSampler = [&](size_t texIndex) -> u32 {
                if (texIndex >= asset.textures.size())
                    return UINT32_MAX;
                const auto& tex = asset.textures[texIndex];
                if (!tex.samplerIndex)
                    return defaultSamplerIndex;
                if (*tex.samplerIndex >= asset.samplers.size())
                    return defaultSamplerIndex;
                return static_cast<u32>(*tex.samplerIndex);
            };

            if (m.pbrData.baseColorTexture)
            {
                const size_t ti = m.pbrData.baseColorTexture->textureIndex;
                mr.baseColorTx = texIndexToTxhd(ti);
                if (mr.baseColorTx >= 0)
                    mr.baseColorSampler = chooseSampler(ti);
            }

            if (m.normalTexture)
            {
                const size_t ti = m.normalTexture->textureIndex;
                mr.normalTx = texIndexToTxhd(ti);
                mr.normalScale = static_cast<f32>(m.normalTexture->scale);
                if (mr.normalTx >= 0)
                    mr.normalSampler = chooseSampler(ti);
            }

            if (m.pbrData.metallicRoughnessTexture)
            {
                const size_t ti = m.pbrData.metallicRoughnessTexture->textureIndex;
                mr.metallicRoughTx = texIndexToTxhd(ti);
                if (mr.metallicRoughTx >= 0)
                    mr.metallicRoughSampler = chooseSampler(ti);
            }

            if (m.occlusionTexture)
            {
                const size_t ti = m.occlusionTexture->textureIndex;
                mr.occlusionTx = texIndexToTxhd(ti);
                mr.occlusionStrength = static_cast<f32>(m.occlusionTexture->strength);
                if (mr.occlusionTx >= 0)
                    mr.occlusionSampler = chooseSampler(ti);
            }

            if (m.emissiveTexture)
            {
                const size_t ti = m.emissiveTexture->textureIndex;
                mr.emissiveTx = texIndexToTxhd(ti);
                if (mr.emissiveTx >= 0)
                    mr.emissiveSampler = chooseSampler(ti);
            }
        }

        outMatl[i] = mr;
    }
}

static void BuildPrimitiveOpacityMicromap(u32 materialIndex, size_t meshIdx, size_t primIdx, bool hasTexcoords, const std::vector<Vertex>& outVertices, const std::vector<u32>& outIndices,
                                          const std::vector<MaskMaterialAlphaSource>& alphaSources, PrimRecord& outPrim, std::vector<i32>& blobOmmIndices,
                                          std::vector<OpacityMicromapDescRecord>& blobOmmDescs, std::vector<u8>& blobOmmData, OMMBuildStats& stats)
{
    if (materialIndex >= alphaSources.size())
        return;

    const MaskMaterialAlphaSource& source = alphaSources[materialIndex];
    if (!source.enabled)
        return;

    if (!hasTexcoords)
    {
        std::println("Error: masked primitive mesh={} prim={} has no TEXCOORD_0 required for OMM generation", meshIdx, primIdx);
        std::exit(EXIT_FAILURE);
    }

    Require((outIndices.size() % 3) == 0, "Primitive OMM index count must be divisible by 3");
    Require(outIndices.size() / 3 <= UINT32_MAX, "Primitive triangle count exceeds OMM pack format limits");
    Require(blobOmmIndices.size() <= UINT32_MAX, "Scene OMM index table exceeds pack format limits");
    Require(blobOmmDescs.size() <= UINT32_MAX, "Scene OMM descriptor table exceeds pack format limits");
    Require(blobOmmData.size() <= UINT64_MAX, "Scene OMM data blob exceeds pack format limits");

    std::vector<XMFLOAT2> ommUvs(outVertices.size());
    for (size_t i = 0; i < outVertices.size(); ++i)
        ommUvs[i] = UnpackTexCoordHalf2(outVertices[i].texCoordPacked);

    const size_t triangleCount = outIndices.size() / 3;
    std::vector<unsigned char> levels(triangleCount);
    std::vector<unsigned int> sources(triangleCount);
    std::vector<int> ommIndices(triangleCount);
    const size_t ommCount = meshopt_opacityMapMeasure(levels.data(), sources.data(), ommIndices.data(), outIndices.data(), outIndices.size(),
                                                      reinterpret_cast<const f32*>(ommUvs.data()), ommUvs.size(), sizeof(XMFLOAT2), source.width, source.height,
                                                      kOpacityMicromapMaxLevel, kOpacityMicromapTargetEdge);

    std::vector<unsigned int> offsets(ommCount);
    size_t dataSizeBeforeCompaction = 0;
    for (size_t i = 0; i < ommCount; ++i)
    {
        Require(dataSizeBeforeCompaction <= UINT32_MAX, "Primitive OMM data exceeds pack format limits");
        offsets[i] = static_cast<unsigned int>(dataSizeBeforeCompaction);
        dataSizeBeforeCompaction += meshopt_opacityMapEntrySize(levels[i], kOpacityMicromapStates);
    }
    Require(dataSizeBeforeCompaction <= UINT32_MAX, "Primitive OMM data exceeds pack format limits");

    std::vector<u8> data(dataSizeBeforeCompaction);
    const u8* const alphaChannel = source.pixels.data() + 3;
    for (size_t i = 0; i < ommCount; ++i)
    {
        const unsigned int tri = sources[i];
        Require(tri < triangleCount, "OMM source triangle index is out of range");
        const f32* uv0 = reinterpret_cast<const f32*>(&ommUvs[outIndices[tri * 3 + 0]]);
        const f32* uv1 = reinterpret_cast<const f32*>(&ommUvs[outIndices[tri * 3 + 1]]);
        const f32* uv2 = reinterpret_cast<const f32*>(&ommUvs[outIndices[tri * 3 + 2]]);
        meshopt_opacityMapRasterize(data.data() + offsets[i], levels[i], kOpacityMicromapStates, uv0, uv1, uv2, alphaChannel, 4, source.rowPitch, source.width, source.height);
    }

    size_t compactedCount = meshopt_opacityMapCompact(data.data(), data.size(), levels.data(), offsets.data(), ommCount, ommIndices.data(), triangleCount, kOpacityMicromapStates);
    const size_t dataSizeAfterCompaction =
        (compactedCount == 0) ? 0 : static_cast<size_t>(offsets[compactedCount - 1]) + meshopt_opacityMapEntrySize(levels[compactedCount - 1], kOpacityMicromapStates);
    Require(compactedCount <= UINT32_MAX, "Primitive OMM entry count exceeds pack format limits");
    Require(dataSizeAfterCompaction <= UINT32_MAX, "Primitive compacted OMM data exceeds pack format limits");
    for (int ommIndex : ommIndices)
    {
        Require(ommIndex >= -kOpacityMicromapStates, "Primitive OMM special index is out of range");
        Require(ommIndex < static_cast<int>(compactedCount), "Primitive OMM index is out of range");
    }

    outPrim.ommIndexOffset = static_cast<u32>(blobOmmIndices.size());
    outPrim.ommIndexCount = static_cast<u32>(triangleCount);
    outPrim.ommDescOffset = static_cast<u32>(blobOmmDescs.size());
    outPrim.ommDescCount = static_cast<u32>(compactedCount);
    outPrim.ommDataByteOffset = blobOmmData.size();
    outPrim.ommDataByteSize = static_cast<u32>(dataSizeAfterCompaction);
    outPrim.ommFormat = kOpacityMicromapStates;

    for (int ommIndex : ommIndices)
        blobOmmIndices.push_back(static_cast<i32>(ommIndex));

    for (size_t i = 0; i < compactedCount; ++i)
    {
        const size_t entrySize = meshopt_opacityMapEntrySize(levels[i], kOpacityMicromapStates);
        Require(offsets[i] <= UINT32_MAX && entrySize <= UINT32_MAX, "Primitive OMM entry exceeds pack format limits");
        OpacityMicromapDescRecord desc{};
        desc.dataByteOffset = offsets[i];
        desc.dataByteSize = static_cast<u32>(entrySize);
        desc.subdivisionLevel = levels[i];
        desc.reserved = 0;
        blobOmmDescs.push_back(desc);
    }
    blobOmmData.insert(blobOmmData.end(), data.begin(), data.begin() + dataSizeAfterCompaction);

    stats.maskedPrimitiveCount += 1;
    stats.entryCount += compactedCount;
    stats.dataBytesBeforeCompaction += dataSizeBeforeCompaction;
    stats.dataBytesAfterCompaction += dataSizeAfterCompaction;
}

static PrimRecord BuildOnePrimitive(const fastgltf::Asset& asset, size_t meshIdx, size_t primIdx, std::vector<Vertex>& blobVertices, std::vector<u32>& blobIndices,
                                    std::vector<IskurMeshlet>& blobMeshlets, std::vector<u32>& blobMLVerts, std::vector<u8>& blobMLTris,
                                    std::vector<MeshletBounds>& blobMLBounds, const std::vector<MaskMaterialAlphaSource>& alphaSources, std::vector<i32>& blobOmmIndices,
                                    std::vector<OpacityMicromapDescRecord>& blobOmmDescs, std::vector<u8>& blobOmmData, OMMBuildStats& ommStats)
{
    const auto& gltfPrim = asset.meshes[meshIdx].primitives[primIdx];
    if (gltfPrim.type != fastgltf::PrimitiveType::Triangles)
        Fatal("Only triangle primitives are supported");

    const u32 materialIndex = gltfPrim.materialIndex ? static_cast<u32>(*gltfPrim.materialIndex) : 0u;

    auto posAttr = gltfPrim.findAttribute("POSITION");
    Require(posAttr != gltfPrim.attributes.end(), "Primitive missing POSITION attribute");
    const auto& posAcc = asset.accessors[posAttr->accessorIndex];
    const size_t positionCount = static_cast<size_t>(posAcc.count);
    Require(positionCount > 0, "Primitive has zero POSITION vertices");

    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;
    std::vector<XMFLOAT4> colors;

    if (auto it = gltfPrim.findAttribute("NORMAL"); it != gltfPrim.attributes.end())
    {
        const auto& acc = asset.accessors[it->accessorIndex];
        Require(static_cast<size_t>(acc.count) == positionCount, "NORMAL count must match POSITION count");
        normals.resize(positionCount);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, acc, [&](fastgltf::math::fvec3 n, std::size_t i) { normals[i] = XMFLOAT3(n.x(), n.y(), n.z()); });
        for (const XMFLOAT3& n : normals)
        {
            Require(IsFiniteFloat3(n), "NORMAL contains NaN/Inf");
        }
    }
    if (auto it = gltfPrim.findAttribute("TEXCOORD_0"); it != gltfPrim.attributes.end())
    {
        const auto& acc = asset.accessors[it->accessorIndex];
        Require(static_cast<size_t>(acc.count) == positionCount, "TEXCOORD_0 count must match POSITION count");
        texcoords.resize(positionCount);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, acc, [&](fastgltf::math::fvec2 uv, std::size_t i) { texcoords[i] = XMFLOAT2(uv.x(), uv.y()); });
        for (const XMFLOAT2& uv : texcoords)
        {
            Require(IsFiniteFloat2(uv), "TEXCOORD_0 contains NaN/Inf");
        }
    }
    if (auto it = gltfPrim.findAttribute("COLOR_0"); it != gltfPrim.attributes.end())
    {
        const auto& acc = asset.accessors[it->accessorIndex];
        Require(static_cast<size_t>(acc.count) == positionCount, "COLOR_0 count must match POSITION count");
        colors.assign(positionCount, XMFLOAT4(1, 1, 1, 1));
        if (acc.type == fastgltf::AccessorType::Vec3)
        {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, acc, [&](fastgltf::math::fvec3 c, std::size_t i) { colors[i] = XMFLOAT4(c.x(), c.y(), c.z(), 1.0f); });
        }
        else if (acc.type == fastgltf::AccessorType::Vec4)
        {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, acc, [&](fastgltf::math::fvec4 c, std::size_t i) { colors[i] = XMFLOAT4(c.x(), c.y(), c.z(), c.w()); });
        }
        else
        {
            Fatal("COLOR_0 must be vec3 or vec4");
        }
        for (const XMFLOAT4& color : colors)
        {
            Require(IsFiniteFloat4(color), "COLOR_0 contains NaN/Inf");
        }
    }

    std::vector<Vertex> initialVertices;
    {
        initialVertices.resize(positionCount);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, posAcc, [&](fastgltf::math::fvec3 pos, std::size_t i) {
            Vertex v{};
            v.position = XMFLOAT3(pos.x(), pos.y(), pos.z());
            Require(IsFiniteFloat3(v.position), "POSITION contains NaN/Inf");
            if (!normals.empty())
            {
                const XMFLOAT3 n = normals[i];
                f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                const XMFLOAT3 nNorm = (len > 0) ? XMFLOAT3(n.x / len, n.y / len, n.z / len) : XMFLOAT3(0, 0, 1);
                v.normalPacked = PackNormalOctSnorm16(nNorm);
            }
            else
            {
                v.normalPacked = PackNormalOctSnorm16(XMFLOAT3(0, 0, 1));
            }
            const XMFLOAT2 uv = (!texcoords.empty()) ? texcoords[i] : XMFLOAT2(0, 0);
            Require(IsFiniteFloat2(uv), "TEXCOORD_0 contains NaN/Inf");
            v.texCoordPacked = PackTexCoordHalf2(uv);
            const XMFLOAT4 color = (!colors.empty()) ? colors[i] : XMFLOAT4(1, 1, 1, 1);
            Require(IsFiniteFloat4(color), "COLOR_0 contains NaN/Inf");
            const PackedColorRGBA16Unorm packedColor = PackColorRGBA16Unorm(color);
            v.colorPackedLo = packedColor.lo;
            v.colorPackedHi = packedColor.hi;
            v.tangentPacked = PackTangentR10G10B10A2(XMFLOAT3(1, 0, 0), 1.0f);
            initialVertices[i] = v;
        });
    }

    std::vector<u32> initialIndices;
    if (gltfPrim.indicesAccessor)
    {
        const auto& acc = asset.accessors[*gltfPrim.indicesAccessor];
        initialIndices.resize(acc.count);
        if (acc.componentType == fastgltf::ComponentType::UnsignedByte)
        {
            std::vector<u8> tmp(acc.count);
            fastgltf::copyFromAccessor<u8>(asset, acc, tmp.data());
            for (size_t i = 0; i < tmp.size(); ++i)
                initialIndices[i] = tmp[i];
        }
        else if (acc.componentType == fastgltf::ComponentType::UnsignedShort)
        {
            std::vector<u16> tmp(acc.count);
            fastgltf::copyFromAccessor<u16>(asset, acc, tmp.data());
            for (size_t i = 0; i < tmp.size(); ++i)
                initialIndices[i] = tmp[i];
        }
        else if (acc.componentType == fastgltf::ComponentType::UnsignedInt)
        {
            fastgltf::copyFromAccessor<u32>(asset, acc, initialIndices.data());
        }
        else
        {
            Fatal("Unsupported index component type");
        }
    }
    else
    {
        initialIndices.resize(initialVertices.size());
        for (u32 i = 0; i < static_cast<u32>(initialVertices.size()); ++i)
            initialIndices[i] = i;
    }

    Require((initialIndices.size() % 3) == 0, "Primitive index count must be divisible by 3");
    const size_t vertexCount = initialVertices.size();
    for (u32 idx : initialIndices)
        Require(static_cast<size_t>(idx) < vertexCount, "Primitive index out of range for POSITION accessor");

    if (normals.empty())
    {
        for (size_t i = 0; i < initialIndices.size(); i += 3)
        {
            u32 i0 = initialIndices[i + 0], i1 = initialIndices[i + 1], i2 = initialIndices[i + 2];
            const XMFLOAT3 &v0 = initialVertices[i0].position, &v1 = initialVertices[i1].position, &v2 = initialVertices[i2].position;
            f32 e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
            f32 e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
            f32 nx = e1y * e2z - e1z * e2y;
            f32 ny = e1z * e2x - e1x * e2z;
            f32 nz = e1x * e2y - e1y * e2x;
            f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0)
            {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            XMFLOAT3 n(nx, ny, nz);
            const u32 packedN = PackNormalOctSnorm16(n);
            initialVertices[i0].normalPacked = packedN;
            initialVertices[i1].normalPacked = packedN;
            initialVertices[i2].normalPacked = packedN;
        }
    }
    if (!texcoords.empty())
        ComputeTangentsMikk(initialIndices, initialVertices);

    std::vector<u32> remap(initialIndices.size());
    const size_t totalVertices = meshopt_generateVertexRemap(remap.data(), initialIndices.data(), initialIndices.size(), initialVertices.data(), initialVertices.size(), sizeof(Vertex));
    std::vector<u32> outIndices(initialIndices.size());
    meshopt_remapIndexBuffer(outIndices.data(), initialIndices.data(), initialIndices.size(), remap.data());
    std::vector<Vertex> outVertices(totalVertices);
    meshopt_remapVertexBuffer(outVertices.data(), initialVertices.data(), initialVertices.size(), sizeof(Vertex), remap.data());
    meshopt_optimizeVertexCache(outIndices.data(), outIndices.data(), outIndices.size(), outVertices.size());
    meshopt_optimizeOverdraw(outIndices.data(), outIndices.data(), outIndices.size(), &outVertices[0].position.x, outVertices.size(), sizeof(Vertex), 1.05f);
    meshopt_optimizeVertexFetch(outVertices.data(), outIndices.data(), outIndices.size(), outVertices.data(), outVertices.size(), sizeof(Vertex));

    constexpr size_t maxVertices = 64, maxTriangles = 126;
    constexpr f32 coneWeight = 0.25f;
    const size_t maxMeshlets = meshopt_buildMeshletsBound(outIndices.size(), maxVertices, maxTriangles);
    std::vector<meshopt_Meshlet> temp(maxMeshlets);
    std::vector<u32> mlVerts(maxMeshlets * maxVertices);
    std::vector<u8> mlTris(maxMeshlets * maxTriangles * 3);
    const size_t meshletCount =
        meshopt_buildMeshlets(temp.data(), mlVerts.data(), mlTris.data(), outIndices.data(), outIndices.size(), &outVertices[0].position.x, outVertices.size(), sizeof(Vertex), maxVertices,
                              maxTriangles, coneWeight);
    temp.resize(meshletCount);
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
        Require(IsFiniteFloat3(mb.center), "Meshlet bounds center contains NaN/Inf");
        Require(IsFiniteF32(mb.radius) && mb.radius >= 0.0f, "Meshlet bounds radius is invalid");
        Require(IsFiniteFloat3(mb.cone_apex) && IsFiniteFloat3(mb.cone_axis) && IsFiniteF32(mb.cone_cutoff), "Meshlet cone data contains NaN/Inf");
        mb.coneAxisAndCutoff = (static_cast<u32>(static_cast<u8>(b.cone_axis_s8[0]))) | (static_cast<u32>(static_cast<u8>(b.cone_axis_s8[1])) << 8) |
                               (static_cast<u32>(static_cast<u8>(b.cone_axis_s8[2])) << 16) | (static_cast<u32>(static_cast<u8>(b.cone_cutoff_s8)) << 24);
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

    XMFLOAT3 localBoundsCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
    f32 localBoundsRadius = 0.0f;
    if (!outVertices.empty())
    {
        XMFLOAT3 minPos = outVertices[0].position;
        XMFLOAT3 maxPos = outVertices[0].position;
        for (const Vertex& v : outVertices)
        {
            minPos.x = std::min(minPos.x, v.position.x);
            minPos.y = std::min(minPos.y, v.position.y);
            minPos.z = std::min(minPos.z, v.position.z);
            maxPos.x = std::max(maxPos.x, v.position.x);
            maxPos.y = std::max(maxPos.y, v.position.y);
            maxPos.z = std::max(maxPos.z, v.position.z);
        }

        localBoundsCenter.x = 0.5f * (minPos.x + maxPos.x);
        localBoundsCenter.y = 0.5f * (minPos.y + maxPos.y);
        localBoundsCenter.z = 0.5f * (minPos.z + maxPos.z);

        f32 maxDistSq = 0.0f;
        for (const Vertex& v : outVertices)
        {
            const f32 dx = v.position.x - localBoundsCenter.x;
            const f32 dy = v.position.y - localBoundsCenter.y;
            const f32 dz = v.position.z - localBoundsCenter.z;
            const f32 distSq = dx * dx + dy * dy + dz * dz;
            maxDistSq = std::max(maxDistSq, distSq);
        }
        localBoundsRadius = std::sqrt(maxDistSq);
    }
    Require(IsFiniteFloat3(localBoundsCenter), "Primitive local bounds center contains NaN/Inf");
    Require(IsFiniteF32(localBoundsRadius) && localBoundsRadius >= 0.0f, "Primitive local bounds radius is invalid");

    PrimRecord r{};
    r.meshIndex = static_cast<u32>(meshIdx);
    r.primIndex = static_cast<u32>(primIdx);
    r.materialIndex = materialIndex;
    Require(outVertices.size() <= UINT32_MAX, "Primitive vertex count exceeds pack format limits");
    Require(outIndices.size() <= UINT32_MAX, "Primitive index count exceeds pack format limits");
    Require(meshlets.size() <= UINT32_MAX, "Primitive meshlet count exceeds pack format limits");
    r.vertexCount = static_cast<u32>(outVertices.size());
    r.indexCount = static_cast<u32>(outIndices.size());
    r.meshletCount = static_cast<u32>(meshlets.size());
    Require(mlVerts.size() <= UINT32_MAX, "Primitive meshlet vertex data exceeds pack format limits");
    Require(mlTris.size() <= UINT32_MAX, "Primitive meshlet triangle data exceeds pack format limits");
    r.mlVertsCount = static_cast<u32>(mlVerts.size());
    r.mlTrisByteCount = static_cast<u32>(mlTris.size());
    r.vertexByteOffset = blobVertices.size() * sizeof(Vertex);
    r.indexByteOffset = blobIndices.size() * sizeof(u32);
    r.meshletsByteOffset = blobMeshlets.size() * sizeof(IskurMeshlet);
    r.mlVertsByteOffset = blobMLVerts.size() * sizeof(u32);
    r.mlTrisByteOffset = blobMLTris.size();
    r.mlBoundsByteOffset = blobMLBounds.size() * sizeof(MeshletBounds);
    r.localBoundsCenter = localBoundsCenter;
    r.localBoundsRadius = localBoundsRadius;
    BuildPrimitiveOpacityMicromap(materialIndex, meshIdx, primIdx, !texcoords.empty(), outVertices, outIndices, alphaSources, r, blobOmmIndices, blobOmmDescs, blobOmmData, ommStats);

    blobVertices.insert(blobVertices.end(), outVertices.begin(), outVertices.end());
    blobIndices.insert(blobIndices.end(), outIndices.begin(), outIndices.end());
    blobMeshlets.insert(blobMeshlets.end(), meshlets.begin(), meshlets.end());
    blobMLVerts.insert(blobMLVerts.end(), mlVerts.begin(), mlVerts.end());
    blobMLTris.insert(blobMLTris.end(), mlTris.begin(), mlTris.end());
    blobMLBounds.insert(blobMLBounds.end(), mlBounds.begin(), mlBounds.end());

    std::println("[prim] mesh={} prim={}  v={} i={} m={} ommEntries={}", r.meshIndex, r.primIndex, r.vertexCount, r.indexCount, r.meshletCount, r.ommDescCount);

    return r;
}

static void BuildMeshPrimToPrimIndex(const std::vector<PrimRecord>& prims, std::vector<std::vector<u32>>& map, size_t meshCount)
{
    map.resize(meshCount);
    for (u32 pi = 0; pi < prims.size(); ++pi)
    {
        const auto& pr = prims[pi];
        auto& vec = map[pr.meshIndex];
        if (vec.size() <= pr.primIndex)
            vec.resize(pr.primIndex + 1, UINT32_MAX);
        vec[pr.primIndex] = pi;
    }
}

static void GatherInstances_Recursive(const fastgltf::Asset& asset, size_t nodeIndex, const std::vector<std::vector<u32>>& meshPrimToPrimIndex, const XMMATRIX& parentWorld,
                                      std::vector<InstanceRecord>& out)
{
    const fastgltf::Node& n = asset.nodes[nodeIndex];
    XMMATRIX world = XMMatrixMultiply(NodeLocalMatrix_Row(n), parentWorld);
    if (n.meshIndex && *n.meshIndex < asset.meshes.size())
    {
        const auto& m = asset.meshes[*n.meshIndex];
        for (size_t p = 0; p < m.primitives.size(); ++p)
        {
            u32 primIndex = UINT32_MAX;
            if (*n.meshIndex < meshPrimToPrimIndex.size())
            {
                const auto& vec = meshPrimToPrimIndex[*n.meshIndex];
                if (p < vec.size())
                    primIndex = vec[p];
            }
            if (primIndex == UINT32_MAX)
                continue;
            InstanceRecord inst{};
            inst.primIndex = primIndex;
            XMStoreFloat4x4(&inst.world, world);
            Require(IsFiniteFloat4x4(inst.world), "Instance world matrix contains NaN/Inf");
            XMVECTOR det{};
            const XMMATRIX worldInv = XMMatrixInverse(&det, world);
            XMFLOAT4X4 worldInv4x4{};
            XMStoreFloat4x4(&worldInv4x4, worldInv);
            Require(IsFiniteF32(XMVectorGetX(det)) && IsFiniteFloat4x4(worldInv4x4), "Instance world matrix is non-invertible");
            out.push_back(inst);
        }
    }
    for (size_t child : n.children)
        if (child < asset.nodes.size())
            GatherInstances_Recursive(asset, child, meshPrimToPrimIndex, world, out);
}

static void BuildInstanceTable(const fastgltf::Asset& asset, const std::vector<PrimRecord>& prims, std::vector<InstanceRecord>& out)
{
    Require(!asset.scenes.empty(), "glTF must contain at least one scene");
    std::vector<std::vector<u32>> map;
    BuildMeshPrimToPrimIndex(prims, map, asset.meshes.size());
    size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex >= asset.scenes.size())
        sceneIndex = 0;
    const XMMATRIX I = XMMatrixIdentity();
    for (size_t nodeIdx : asset.scenes[sceneIndex].nodeIndices)
        if (nodeIdx < asset.nodes.size())
            GatherInstances_Recursive(asset, nodeIdx, map, I, out);
}

static void ResolveInstanceMaterials(const std::vector<PrimRecord>& prims, const std::vector<MaterialRecord>& mats, std::vector<InstanceRecord>& inst)
{
    const u32 matCount = static_cast<u32>(mats.empty() ? 1 : mats.size());

    for (auto& I : inst)
    {
        u32 mat = (I.primIndex < prims.size()) ? prims[I.primIndex].materialIndex : 0u;
        if (mat >= matCount)
            mat = 0u;
        I.materialIndex = mat;
    }
}

static void ProcessAllMeshesAndWritePack(const fs::path& outPackPath, const fs::path& glbPath, const fastgltf::Asset& asset, bool fastCompress)
{
    std::vector<PrimRecord> prims;
    std::vector<Vertex> blobVertices;
    std::vector<u32> blobIndices;
    std::vector<IskurMeshlet> blobMeshlets;
    std::vector<u32> blobMLVerts;
    std::vector<u8> blobMLTris;
    std::vector<MeshletBounds> blobMLBounds;
    std::vector<i32> blobOmmIndices;
    std::vector<OpacityMicromapDescRecord> blobOmmDescs;
    std::vector<u8> blobOmmData;
    const std::vector<MaskMaterialAlphaSource> alphaSources = BuildMaskMaterialAlphaSources(glbPath, asset);
    OMMBuildStats ommStats{};

    for (size_t mi = 0; mi < asset.meshes.size(); ++mi)
    {
        const auto& mesh = asset.meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi)
        {
            PrimRecord r =
                BuildOnePrimitive(asset, mi, pi, blobVertices, blobIndices, blobMeshlets, blobMLVerts, blobMLTris, blobMLBounds, alphaSources, blobOmmIndices, blobOmmDescs, blobOmmData, ommStats);
            prims.push_back(r);
        }
    }

    std::vector<InstanceRecord> instTable;
    BuildInstanceTable(asset, prims, instTable);

    std::vector<TextureRecord> texTable;
    std::vector<TextureSubresourceRecord> texSubresources;
    std::vector<u8> texBlob;
    {
        auto imgUsage = BuildImageUsageFlags(asset);
        BuildTexturesToMemory(glbPath, asset, imgUsage, fastCompress, texTable, texSubresources, texBlob);
        if (!asset.textures.empty() && texTable.empty())
            Fatal("Texture table is empty despite glTF having textures");
    }

    std::vector<D3D12_SAMPLER_DESC> sampTable;
    std::vector<MaterialRecord> matTable;
    const u32 defaultSamplerIndex = BuildSamplerTable(asset, sampTable);
    BuildMaterialTable(asset, texTable, matTable, defaultSamplerIndex);

    if (!instTable.empty())
        ResolveInstanceMaterials(prims, matTable, instTable);

    std::vector<ChunkRecord> chunks;
    u32 chunkCount = 10u + (texTable.empty() ? 0u : 3u) + (sampTable.empty() ? 0u : 1u) + (matTable.empty() ? 0u : 1u) + (instTable.empty() ? 0u : 1u);

    const u64 ofsHeader = 0;
    const u64 ofsChunkTbl = ofsHeader + sizeof(PackHeader);
    const u64 ofsPrimTbl = ofsChunkTbl + chunkCount * sizeof(ChunkRecord);
    const u64 ofsVertices = ofsPrimTbl + prims.size() * sizeof(PrimRecord);
    const u64 ofsIndices = ofsVertices + blobVertices.size() * sizeof(Vertex);
    const u64 ofsMeshlets = ofsIndices + blobIndices.size() * sizeof(u32);
    const u64 ofsMLVerts = ofsMeshlets + blobMeshlets.size() * sizeof(IskurMeshlet);
    const u64 ofsMLTris = ofsMLVerts + blobMLVerts.size() * sizeof(u32);
    const u64 ofsMLBounds = ofsMLTris + blobMLTris.size();
    const u64 ofsOmmIndices = ofsMLBounds + blobMLBounds.size() * sizeof(MeshletBounds);
    const u64 ofsOmmDescs = ofsOmmIndices + blobOmmIndices.size() * sizeof(i32);
    const u64 ofsOmmData = ofsOmmDescs + blobOmmDescs.size() * sizeof(OpacityMicromapDescRecord);
    u64 cur = ofsOmmData + blobOmmData.size();

    const u64 ofsTxHd = texTable.empty() ? 0 : cur;
    if (!texTable.empty())
        cur += texTable.size() * sizeof(TextureRecord);
    const u64 ofsTxSr = texTable.empty() ? 0 : cur;
    if (!texTable.empty())
        cur += texSubresources.size() * sizeof(TextureSubresourceRecord);
    const u64 ofsTxTb = texTable.empty() ? 0 : cur;
    if (!texTable.empty())
        cur += texBlob.size();
    const u64 ofsSamp = sampTable.empty() ? 0 : cur;
    if (!sampTable.empty())
        cur += sampTable.size() * sizeof(D3D12_SAMPLER_DESC);
    const u64 ofsMatl = matTable.empty() ? 0 : cur;
    if (!matTable.empty())
        cur += matTable.size() * sizeof(MaterialRecord);
    const u64 ofsInst = instTable.empty() ? 0 : cur;
    if (!instTable.empty())
        cur += instTable.size() * sizeof(InstanceRecord);

    auto addChunk = [&](u32 id, u64 off, u64 size) {
        ChunkRecord r;
        r.id = id;
        r.offset = off;
        r.size = size;
        chunks.push_back(r);
    };

    addChunk(CH_PRIM, ofsPrimTbl, prims.size() * sizeof(PrimRecord));
    addChunk(CH_VERT, ofsVertices, blobVertices.size() * sizeof(Vertex));
    addChunk(CH_INDX, ofsIndices, blobIndices.size() * sizeof(u32));
    addChunk(CH_MSHL, ofsMeshlets, blobMeshlets.size() * sizeof(IskurMeshlet));
    addChunk(CH_MLVT, ofsMLVerts, blobMLVerts.size() * sizeof(u32));
    addChunk(CH_MLTR, ofsMLTris, blobMLTris.size());
    addChunk(CH_MLBD, ofsMLBounds, blobMLBounds.size() * sizeof(MeshletBounds));
    addChunk(CH_OMIX, ofsOmmIndices, blobOmmIndices.size() * sizeof(i32));
    addChunk(CH_OMDS, ofsOmmDescs, blobOmmDescs.size() * sizeof(OpacityMicromapDescRecord));
    addChunk(CH_OMDT, ofsOmmData, blobOmmData.size());

    if (!texTable.empty())
    {
        addChunk(CH_TXHD, ofsTxHd, texTable.size() * sizeof(TextureRecord));
        addChunk(CH_TXSR, ofsTxSr, texSubresources.size() * sizeof(TextureSubresourceRecord));
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

    PackHeader hdr{};
    std::memcpy(hdr.magic, "ISKURPACK", 9);
    hdr.version = PACK_VERSION_LATEST;
    hdr.primCount = static_cast<u32>(prims.size());
    hdr.chunkCount = static_cast<u32>(chunks.size());
    hdr.reserved0 = 0;
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
        Fatal("Failed to open output pack file");

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(chunks.data()), static_cast<std::streamsize>(chunks.size() * sizeof(ChunkRecord)));
    out.write(reinterpret_cast<const char*>(prims.data()), static_cast<std::streamsize>(prims.size() * sizeof(PrimRecord)));
    out.write(reinterpret_cast<const char*>(blobVertices.data()), static_cast<std::streamsize>(blobVertices.size() * sizeof(Vertex)));
    out.write(reinterpret_cast<const char*>(blobIndices.data()), static_cast<std::streamsize>(blobIndices.size() * sizeof(u32)));
    out.write(reinterpret_cast<const char*>(blobMeshlets.data()), static_cast<std::streamsize>(blobMeshlets.size() * sizeof(IskurMeshlet)));
    out.write(reinterpret_cast<const char*>(blobMLVerts.data()), static_cast<std::streamsize>(blobMLVerts.size() * sizeof(u32)));
    out.write(reinterpret_cast<const char*>(blobMLTris.data()), static_cast<std::streamsize>(blobMLTris.size()));
    out.write(reinterpret_cast<const char*>(blobMLBounds.data()), static_cast<std::streamsize>(blobMLBounds.size() * sizeof(MeshletBounds)));
    out.write(reinterpret_cast<const char*>(blobOmmIndices.data()), static_cast<std::streamsize>(blobOmmIndices.size() * sizeof(i32)));
    out.write(reinterpret_cast<const char*>(blobOmmDescs.data()), static_cast<std::streamsize>(blobOmmDescs.size() * sizeof(OpacityMicromapDescRecord)));
    out.write(reinterpret_cast<const char*>(blobOmmData.data()), static_cast<std::streamsize>(blobOmmData.size()));
    if (!texTable.empty())
    {
        out.write(reinterpret_cast<const char*>(texTable.data()), static_cast<std::streamsize>(texTable.size() * sizeof(TextureRecord)));
        out.write(reinterpret_cast<const char*>(texSubresources.data()), static_cast<std::streamsize>(texSubresources.size() * sizeof(TextureSubresourceRecord)));
        out.write(reinterpret_cast<const char*>(texBlob.data()), static_cast<std::streamsize>(texBlob.size()));
    }
    if (!sampTable.empty())
        out.write(reinterpret_cast<const char*>(sampTable.data()), static_cast<std::streamsize>(sampTable.size() * sizeof(D3D12_SAMPLER_DESC)));
    if (!matTable.empty())
        out.write(reinterpret_cast<const char*>(matTable.data()), static_cast<std::streamsize>(matTable.size() * sizeof(MaterialRecord)));
    if (!instTable.empty())
        out.write(reinterpret_cast<const char*>(instTable.data()), static_cast<std::streamsize>(instTable.size() * sizeof(InstanceRecord)));

    if (!out.good())
        Fatal("Error writing pack file");

    std::println("Meshes pack written: {}", outPackPath.string());
    std::println("  prims={}, verts={}, inds={}, meshlets={}, mlVerts={}, mlTris={} bytes, mlBounds={}", static_cast<size_t>(hdr.primCount), blobVertices.size(), blobIndices.size(),
                 blobMeshlets.size(), blobMLVerts.size(), blobMLTris.size(), blobMLBounds.size());
    std::println("  omm: maskedPrims={}, indices={}, entries={}, bytes(before={} after={})", ommStats.maskedPrimitiveCount, blobOmmIndices.size(), ommStats.entryCount,
                 ommStats.dataBytesBeforeCompaction, ommStats.dataBytesAfterCompaction);
    if (!texTable.empty())
        std::println("  textures: {}", texTable.size());
    if (!sampTable.empty())
        std::println("  samplers: {}", sampTable.size());
    if (!matTable.empty())
        std::println("  materials: {}", matTable.size());
    if (!instTable.empty())
        std::println("  instances: {}", instTable.size());
}

static void PrintUsage()
{
    std::println("IskurScenePacker\nUsage:\n  IskurScenePacker --scene <scene> [--fast]\n  IskurScenePacker --all [--fast]");
}

static void WriteIskurScene(const fs::path& inGlb, const fs::path& outPack, bool fastCompress)
{
    constexpr auto supportedExtensions = fastgltf::Extensions::None;
    constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages;

    fastgltf::Parser parser(supportedExtensions);
    fastgltf::Asset model{};

    auto loadAsset = [&](fastgltf::GltfDataGetter& input) {
        auto asset = parser.loadGltf(input, inGlb.parent_path(), gltfOptions);
        if (asset.error() != fastgltf::Error::None)
        {
            std::println("fastgltf load error for '{}': {} ({})", inGlb.string(), fastgltf::getErrorName(asset.error()), fastgltf::getErrorMessage(asset.error()));
            Fatal("Failed to load GLB with fastgltf");
        }
        model = std::move(asset.get());
    };

    auto gltfFile = fastgltf::MappedGltfFile::FromPath(inGlb);
    if (gltfFile)
    {
        loadAsset(gltfFile.get());
    }
    else
    {
        std::println("fastgltf mapped file open failed for '{}': {} ({})", inGlb.string(), fastgltf::getErrorName(gltfFile.error()),
                     fastgltf::getErrorMessage(gltfFile.error()));
        fastgltf::GltfFileStream gltfStream(inGlb);
        if (!gltfStream.isOpen())
            Fatal("Failed to open GLB file");
        loadAsset(gltfStream);
    }

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
        std::string a = ToLowerAscii(argv[i]);
        if ((a == "-i" || a == "--scene") && i + 1 < argc)
            inSceneName = argv[++i];
        else if (a == "--fast")
            fast = true;
        else if (a == "--all")
            processAll = true;
        else
        {
            std::println("Error: Unknown command-line argument '{}'", a);
            std::exit(EXIT_FAILURE);
        }
    }

    if (processAll)
    {
        const fs::path repoRoot = FindRepoRoot(fs::path(argv[0]));
        const fs::path srcRoot = repoRoot / "data" / "scenes" / "sources";
        const fs::path outRoot = repoRoot / "data" / "scenes";
        Require(fs::exists(srcRoot) && fs::is_directory(srcRoot), "Sources directory must exist");
        if (!fs::exists(outRoot))
        {
            std::error_code ec;
            fs::create_directories(outRoot, ec);
            if (ec)
                Fatal("Failed to create output directory");
        }
        int total = 0, okc = 0, skipped = 0;
        for (const auto& de : fs::directory_iterator(srcRoot))
        {
            if (!de.is_regular_file() || !EqualsIgnoreCaseAscii(de.path().extension().string(), ".glb"))
                continue;
            std::string stem = de.path().stem().string();
            fs::path glb = de.path();
            fs::path pack = outRoot / (stem + PACK_FILE_EXTENSION);
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
        f64 sec = std::chrono::duration<f64>(t1 - t0).count();
        std::println("Total time: {:.3f} s", sec);
        return 0;
    }

    if (inSceneName.empty())
    {
        PrintUsage();
        Fatal("No scene specified; use --scene <name> or --all");
    }

    const fs::path repoRoot = FindRepoRoot(fs::path(argv[0]));
    const fs::path srcRoot = repoRoot / "data" / "scenes" / "sources";
    const fs::path outRoot = repoRoot / "data" / "scenes";

    fs::path glbPath;
    if (!ResolveSceneSourcePath(srcRoot, inSceneName.string(), glbPath))
        Fatal("Scene source GLB not found (case-insensitive) in data/scenes/sources");
    const std::string sceneStem = glbPath.stem().string();
    const fs::path outPath = outRoot / (sceneStem + PACK_FILE_EXTENSION);

    {
        const auto outDir = outPath.parent_path();
        if (!fs::exists(outDir))
        {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            if (ec)
                Fatal("Failed to create output directory");
        }
    }

    WriteIskurScene(glbPath, outPath, fast);

    CoUninitialize();

    auto t1 = std::chrono::steady_clock::now();
    f64 sec = std::chrono::duration<f64>(t1 - t0).count();
    std::println("Total time: {:.3f} s", sec);

    return 0;
}

