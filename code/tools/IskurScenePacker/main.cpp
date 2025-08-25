// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include <DirectXMesh.h>
#include <DirectXTex.h>
#include <Objbase.h>
#include <algorithm>
#include <atomic>
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
#include <vector>

// -------------------------------- POD math / engine-compatible PODs --------------------------------
struct float2
{
    float x, y;
};
struct float3
{
    float x, y, z;
};
struct float4
{
    float x, y, z, w;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

struct Meshlet
{
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint16_t vertexCount;
    uint16_t triangleCount;
};

struct MeshletBounds
{
    float3 center;
    float radius;
    float3 cone_apex;
    float3 cone_axis;
    float cone_cutoff;
    uint32_t coneAxisAndCutoff;
};

#pragma pack(push, 1)
struct PackHeader
{
    char magic[8]; // "ISKURPK"
    uint32_t version;
    uint32_t primCount;

    uint64_t primTableOffset;
    uint64_t verticesOffset;
    uint64_t indicesOffset;
    uint64_t meshletsOffset;
    uint64_t mlVertsOffset;
    uint64_t mlTrisOffset;
    uint64_t mlBoundsOffset;
};

struct PrimRecord
{
    uint32_t meshIndex;
    uint32_t primIndex;
    uint32_t materialIndex;

    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t meshletCount;

    uint64_t vertexByteOffset;
    uint64_t indexByteOffset;

    uint64_t meshletsByteOffset;
    uint64_t mlVertsByteOffset;
    uint64_t mlTrisByteOffset;
    uint64_t mlBoundsByteOffset;

    uint32_t mlVertsCount;
    uint32_t mlTrisByteCount;
};
#pragma pack(pop)

static const char* DxgiFormatTag(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_BC5_UNORM: {
        return "BC5";
    }
    case DXGI_FORMAT_BC7_UNORM: {
        return "BC7";
    }
    case DXGI_FORMAT_BC7_UNORM_SRGB: {
        return "BC7_SRGB";
    }
    default: {
        return "UNKNOWN";
    }
    }
}

static bool IsGlbFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
    {
        return false;
    }
    char magic[4] = {};
    f.read(magic, 4);
    if (!f)
    {
        return false;
    }
    return (magic[0] == 'g' && magic[1] == 'l' && magic[2] == 'T' && magic[3] == 'F');
}

static HRESULT LoadAnyImageMemory(const uint8_t* bytes, size_t size, DirectX::ScratchImage& out)
{
    using namespace DirectX;

    if (size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
    {
        return LoadFromDDSMemory(bytes, size, DDS_FLAGS_NONE, nullptr, out);
    }

    if (size >= 10 && std::memcmp(bytes, "#?RADIANCE", 10) == 0)
    {
        return LoadFromHDRMemory(bytes, size, nullptr, out);
    }

    return LoadFromWICMemory(bytes, size, WIC_FLAGS_NONE, nullptr, out);
}

enum : uint32_t
{
    IMG_BASECOLOR = 1u << 0,
    IMG_NORMAL = 1u << 1,
    IMG_METALROUGH = 1u << 2,
    IMG_OCCLUSION = 1u << 3,
    IMG_EMISSIVE = 1u << 4,
};

static std::vector<uint32_t> BuildImageUsageFlags(const tinygltf::Model& model)
{
    std::vector<uint32_t> flags(model.images.size(), 0);

    auto mark = [&](int texIndex, uint32_t f) {
        if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size()))
        {
            return;
        }
        int img = model.textures[size_t(texIndex)].source;
        if (img < 0 || img >= static_cast<int>(flags.size()))
        {
            return;
        }
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

static uint32_t AccessorStrideBytes(const tinygltf::Accessor& accessor, const tinygltf::BufferView& view)
{
    uint32_t s = accessor.ByteStride(view);
    if (s)
    {
        return s;
    }
    const int comps = tinygltf::GetNumComponentsInType(accessor.type);
    const int csize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    return uint32_t(comps * csize);
}

static int mk_getNumFaces(const SMikkTSpaceContext* ctx);
static int mk_getNumVerticesOfFace(const SMikkTSpaceContext* ctx, const int f);
static void mk_getPosition(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v);
static void mk_getNormal(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v);
static void mk_getTexCoord(const SMikkTSpaceContext* ctx, float out[2], const int f, const int v);
static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const float t[3], const float sign, const int f, const int v);

struct MikkUserData
{
    const std::vector<uint32_t>* indices;
    std::vector<Vertex>* verts;
};

static void ComputeTangentsMikk(const std::vector<uint32_t>& indices, std::vector<Vertex>& verts)
{
    // Mikk requires normals & uvs present and normalized.
    // (You already ensure normals exist earlier; keep that.)
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = mk_getNumFaces;
    iface.m_getNumVerticesOfFace = mk_getNumVerticesOfFace;
    iface.m_getPosition = mk_getPosition;
    iface.m_getNormal = mk_getNormal;
    iface.m_getTexCoord = mk_getTexCoord;
    iface.m_setTSpaceBasic = mk_setTSpaceBasic;
    // (m_setTSpace can be null since we don't need per-vertex bitangent)

    MikkUserData ud{&indices, &verts};

    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &ud;

    // This will call our setters and fill Vertex.tangent (xyz + sign in .w)
    genTangSpaceDefault(&ctx);
}

static int mk_getNumFaces(const SMikkTSpaceContext* ctx)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    return int(ud->indices->size() / 3);
}
static int mk_getNumVerticesOfFace(const SMikkTSpaceContext*, const int)
{
    return 3;
}

static void mk_getPosition(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t idx = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    const auto& P = (*ud->verts)[idx].position;
    out[0] = P.x;
    out[1] = P.y;
    out[2] = P.z;
}
static void mk_getNormal(const SMikkTSpaceContext* ctx, float out[3], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t idx = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    const auto& N = (*ud->verts)[idx].normal;
    out[0] = N.x;
    out[1] = N.y;
    out[2] = N.z;
}
static void mk_getTexCoord(const SMikkTSpaceContext* ctx, float out[2], const int f, const int v)
{
    auto* ud = static_cast<const MikkUserData*>(ctx->m_pUserData);
    uint32_t idx = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    const auto& uv = (*ud->verts)[idx].texCoord;
    out[0] = uv.x;
    out[1] = uv.y; // no V flip; your sampling matches glTF UVs
}

// Mikk gives: tangent T and fSign such that B = fSign * cross(N, T)
static void mk_setTSpaceBasic(const SMikkTSpaceContext* ctx, const float t[3], const float fSign, const int f, const int v)
{
    auto* ud = static_cast<MikkUserData*>(ctx->m_pUserData);
    uint32_t idx = (*ud->indices)[size_t(f) * 3 + size_t(v)];
    auto& dst = (*ud->verts)[idx].tangent;
    dst.x = t[0];
    dst.y = t[1];
    dst.z = t[2];
    dst.w = -fSign; // +1 or -1 handedness
}

struct RunOptions
{
    bool doTextures = true;
    bool doMeshes = true;
    bool fast = false;
};

static bool ProcessAllTextures(const std::filesystem::path& glbPath, const tinygltf::Model& model, const std::vector<uint32_t>& imgUsage, const RunOptions& opt)
{
    namespace fs = std::filesystem;
    using namespace DirectX;

    const fs::path baseDir = glbPath.parent_path();
    const fs::path outDir = baseDir / "textures";
    std::error_code ec;
    fs::create_directories(outDir, ec);

    const size_t totalImages = model.images.size();
    if (totalImages == 0)
    {
        std::fprintf(stdout, "No images in GLB.\n");
        return true;
    }

    std::atomic_size_t converted{0}, skipped{0}, failed{0};

    std::vector<size_t> idx(totalImages);
    std::iota(idx.begin(), idx.end(), size_t(0));

    static std::mutex sLog;

    std::for_each(std::execution::par_unseq, idx.begin(), idx.end(), [&](size_t i) {
        const auto& img = model.images[i];

        if (!img.uri.empty() || img.bufferView < 0)
        {
            skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto& bv = model.bufferViews[size_t(img.bufferView)];
        const auto& buf = model.buffers[size_t(bv.buffer)];
        if (size_t(bv.byteOffset) + size_t(bv.byteLength) > buf.data.size())
        {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const uint8_t* p = buf.data.data() + size_t(bv.byteOffset);
        size_t sz = size_t(bv.byteLength);

        ScratchImage loaded;
        if (FAILED(LoadAnyImageMemory(p, sz, loaded)))
        {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const bool isNormal = (i < imgUsage.size()) && ((imgUsage[i] & IMG_NORMAL) != 0);
        bool isSRGB = !isNormal;
        if (!isNormal)
        {
            const bool srgbSem = (i < imgUsage.size()) && ((imgUsage[i] & (IMG_BASECOLOR | IMG_EMISSIVE)) != 0);
            const bool linearSem = (i < imgUsage.size()) && ((imgUsage[i] & (IMG_METALROUGH | IMG_OCCLUSION)) != 0);
            isSRGB = srgbSem && !linearSem;
        }

        ScratchImage convertedImage;
        DXGI_FORMAT wantBase = isSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        const Image* srcImages = nullptr;
        size_t srcCount = 0;
        TexMetadata meta{};

        if (loaded.GetMetadata().format != wantBase)
        {
            if (FAILED(Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(), wantBase, TEX_FILTER_DEFAULT, 0.0f, convertedImage)))
            {
                failed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            srcImages = convertedImage.GetImages();
            srcCount = convertedImage.GetImageCount();
            meta = convertedImage.GetMetadata();
        }
        else
        {
            srcImages = loaded.GetImages();
            srcCount = loaded.GetImageCount();
            meta = loaded.GetMetadata();
        }

        ScratchImage mipChain;
        if (SUCCEEDED(GenerateMipMaps(srcImages, srcCount, meta, TEX_FILTER_DEFAULT, 0, mipChain)))
        {
            srcImages = mipChain.GetImages();
            srcCount = mipChain.GetImageCount();
            meta = mipChain.GetMetadata();
        }

        DXGI_FORMAT compFmt = isNormal ? DXGI_FORMAT_BC5_UNORM : (isSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM);

        TEX_COMPRESS_FLAGS compFlags = TEX_COMPRESS_PARALLEL;
        if (!isNormal && opt.fast)
        {
            compFlags |= TEX_COMPRESS_BC7_QUICK;
        }

        ScratchImage bc;
        if (FAILED(Compress(srcImages, srcCount, meta, compFmt, compFlags, 0.5f, bc)))
        {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::wstring wname = std::to_wstring(i) + L".dds";
        const fs::path dst = outDir / wname;
        if (FAILED(SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_FORCE_DX10_EXT, dst.c_str())))
        {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            std::lock_guard<std::mutex> _g(sLog);
            std::fprintf(stdout, "[tex] (%zu/%zu) %zu.dds  format=%s\n", i + 1, totalImages, i, DxgiFormatTag(compFmt));
            std::fflush(stdout);
        }

        converted.fetch_add(1, std::memory_order_relaxed);
    });

    std::fprintf(stdout, "Textures done. converted=%zu, skipped=%zu, failed=%zu (fast=%s)\n", size_t(converted), size_t(skipped), size_t(failed), opt.fast ? "yes" : "no");
    return failed == 0;
}

struct BuiltPrimitive
{
    uint32_t meshIndex = 0;
    uint32_t primIndex = 0;
    uint32_t materialIndex = 0;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Meshlet> meshlets;
    std::vector<uint32_t> mlVerts;
    std::vector<uint8_t> mlTris;
    std::vector<MeshletBounds> mlBounds;
};

static void BuildOnePrimitive(const tinygltf::Model& model, int meshIdx, int primIdx, BuiltPrimitive& out)
{
    const auto& gltfPrim = model.meshes[size_t(meshIdx)].primitives[size_t(primIdx)];
    if (gltfPrim.mode != TINYGLTF_MODE_TRIANGLES)
    {
        std::fprintf(stderr, "Non-triangle primitive encountered. Aborting.\n");
        std::abort();
    }

    out.meshIndex = uint32_t(meshIdx);
    out.primIndex = uint32_t(primIdx);
    out.materialIndex = (gltfPrim.material >= 0) ? uint32_t(gltfPrim.material) : 0;

    std::vector<float3> normals;
    std::vector<float2> texcoords;
    std::vector<float4> tangents;

    if (gltfPrim.attributes.contains("NORMAL"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("NORMAL"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        const uint32_t count = uint32_t(acc.count);
        const uint32_t stride = AccessorStrideBytes(acc, view);

        normals.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&normals[i], data + i * stride, sizeof(float3));
        }
    }

    if (gltfPrim.attributes.contains("TEXCOORD_0"))
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("TEXCOORD_0"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        const uint32_t count = uint32_t(acc.count);
        const uint32_t stride = AccessorStrideBytes(acc, view);

        texcoords.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&texcoords[i], data + i * stride, sizeof(float2));
        }
    }

    if (gltfPrim.attributes.contains("TEXCOORD_1"))
    {
        assert(false);
    }

    std::vector<Vertex> initialVertices;
    {
        const auto& acc = model.accessors.at(gltfPrim.attributes.at("POSITION"));
        const auto& view = model.bufferViews[size_t(acc.bufferView)];
        const auto& buf = model.buffers[size_t(view.buffer)];
        const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;
        const uint32_t count = uint32_t(acc.count);
        const uint32_t stride = AccessorStrideBytes(acc, view);

        initialVertices.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Vertex v{};
            std::memcpy(&v.position, data + i * stride, sizeof(float3));

            if (!normals.empty())
            {
                float3 n = normals[i];
                float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                v.normal = (len > 0.0f) ? float3{n.x / len, n.y / len, n.z / len} : float3{0, 0, 1};
            }
            else
            {
                v.normal = float3{0, 0, 0}; // will be filled by face-normal path below
            }

            v.texCoord = (!texcoords.empty()) ? texcoords[i] : float2{0, 0};
            v.tangent = float4{0, 0, 0, 0};
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
        const uint32_t count = uint32_t(acc.count);

        initialIndices.resize(count);
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                initialIndices[i] = uint32_t(data[i]);
            }
        }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
            {
                initialIndices[i] = uint32_t(p[i]);
            }
        }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
            {
                initialIndices[i] = p[i];
            }
        }
        else
        {
            std::fprintf(stderr, "Unsupported index component type. Aborting.\n");
            std::abort();
        }
    }
    else
    {
        initialIndices.resize(initialVertices.size());
        for (uint32_t i = 0; i < uint32_t(initialVertices.size()); ++i)
        {
            initialIndices[i] = i;
        }
    }

    if (normals.empty())
    {
        for (size_t i = 0; i < initialIndices.size(); i += 3)
        {
            uint32_t i0 = initialIndices[i + 0];
            uint32_t i1 = initialIndices[i + 1];
            uint32_t i2 = initialIndices[i + 2];
            const float3& v0 = initialVertices[i0].position;
            const float3& v1 = initialVertices[i1].position;
            const float3& v2 = initialVertices[i2].position;
            float3 e1{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
            float3 e2{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
            float3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 0.0f)
            {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            }
            initialVertices[i0].normal = n;
            initialVertices[i1].normal = n;
            initialVertices[i2].normal = n;
        }
    }

    if (!texcoords.empty())
    {
        ComputeTangentsMikk(initialIndices, initialVertices);
    }

    std::vector<uint32_t> remap(initialIndices.size());
    uint32_t totalVertices = meshopt_generateVertexRemap(remap.data(), initialIndices.data(), initialIndices.size(), initialVertices.data(), initialVertices.size(), sizeof(Vertex));

    out.indices.resize(initialIndices.size());
    meshopt_remapIndexBuffer(out.indices.data(), initialIndices.data(), initialIndices.size(), remap.data());

    out.vertices.resize(totalVertices);
    meshopt_remapVertexBuffer(out.vertices.data(), initialVertices.data(), initialVertices.size(), sizeof(Vertex), remap.data());

    meshopt_optimizeVertexCache(out.indices.data(), out.indices.data(), out.indices.size(), out.vertices.size());
    meshopt_optimizeOverdraw(out.indices.data(), out.indices.data(), out.indices.size(), &out.vertices[0].position.x, out.vertices.size(), sizeof(Vertex), 1.05f);
    meshopt_optimizeVertexFetch(out.vertices.data(), out.indices.data(), out.indices.size(), out.vertices.data(), out.vertices.size(), sizeof(Vertex));

    constexpr uint64_t maxVertices = 64;
    constexpr uint64_t maxTriangles = 124;
    constexpr float coneWeight = 0.f;

    const uint32_t maxMeshlets = uint32_t(meshopt_buildMeshletsBound(out.indices.size(), maxVertices, maxTriangles));
    std::vector<meshopt_Meshlet> temp(maxMeshlets);

    out.mlVerts.resize(maxMeshlets * maxVertices);
    out.mlTris.resize(maxMeshlets * maxTriangles * 3);

    uint32_t meshletsCount = uint32_t(meshopt_buildMeshlets(temp.data(), out.mlVerts.data(), out.mlTris.data(), out.indices.data(), out.indices.size(), &out.vertices[0].position.x,
                                                            out.vertices.size(), sizeof(Vertex), maxVertices, maxTriangles, coneWeight));

    (void)meshletsCount;

    temp.resize(meshletsCount);
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
        mb.center = {b.center[0], b.center[1], b.center[2]};
        mb.radius = b.radius;
        mb.cone_apex = {b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]};
        mb.cone_axis = {b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]};
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
        out.meshlets[i].vertexCount = static_cast<uint16_t>(temp[i].vertex_count);
        out.meshlets[i].triangleCount = static_cast<uint16_t>(temp[i].triangle_count);
    }
}

static bool ProcessAllMeshesAndWritePack(const std::filesystem::path& outPackPath, const tinygltf::Model& model)
{
    using clock = std::chrono::steady_clock;

    struct PrimJob
    {
        int meshIdx;
        int primIdx;
        size_t outIdx;
    };

    // Plan jobs
    size_t totalPrim = 0;
    for (int mi = 0; mi < (int)model.meshes.size(); ++mi)
        totalPrim += model.meshes[size_t(mi)].primitives.size();

    std::vector<PrimJob> jobs;
    jobs.reserve(totalPrim);
    for (int mi = 0; mi < (int)model.meshes.size(); ++mi)
    {
        const auto& mesh = model.meshes[size_t(mi)];
        for (int pi = 0; pi < (int)mesh.primitives.size(); ++pi)
            jobs.push_back(PrimJob{mi, pi, jobs.size()});
    }

    // Build in parallel with live logging
    std::vector<BuiltPrimitive> built(jobs.size());
    std::atomic_size_t done{0};
    std::mutex logMtx;

    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](const PrimJob& j) {
        const auto tStart = clock::now();
        BuildOnePrimitive(model, j.meshIdx, j.primIdx, built[j.outIdx]);
        const auto tEnd = clock::now();

        const double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        const size_t d = done.fetch_add(1, std::memory_order_relaxed) + 1;

        {
            std::lock_guard<std::mutex> _g(logMtx);
            const auto& bp = built[j.outIdx];
            std::printf("[prim][%zu/%zu] done   mesh=%u prim=%u  verts=%zu idx=%zu meshlets=%zu  %.1f ms\n", d, jobs.size(), bp.meshIndex, bp.primIndex, +bp.vertices.size(), bp.indices.size(),
                        bp.meshlets.size(), ms);
            std::fflush(stdout);
        }
    });

    // Pack (serial, unchanged logic)
    std::vector<PrimRecord> prims;
    std::vector<Vertex> blobVertices;
    std::vector<uint32_t> blobIndices;
    std::vector<Meshlet> blobMeshlets;
    std::vector<uint32_t> blobMLVerts;
    std::vector<uint8_t> blobMLTris;
    std::vector<MeshletBounds> blobMLBounds;

    prims.reserve(built.size());

    for (const auto& b : built)
    {
        PrimRecord r{};
        r.meshIndex = b.meshIndex;
        r.primIndex = b.primIndex;
        r.materialIndex = b.materialIndex;

        r.vertexCount = uint32_t(b.vertices.size());
        r.indexCount = uint32_t(b.indices.size());
        r.meshletCount = uint32_t(b.meshlets.size());
        r.mlVertsCount = uint32_t(b.mlVerts.size());
        r.mlTrisByteCount = uint32_t(b.mlTris.size());

        r.vertexByteOffset = uint64_t(blobVertices.size()) * sizeof(Vertex);
        r.indexByteOffset = uint64_t(blobIndices.size()) * sizeof(uint32_t);
        r.meshletsByteOffset = uint64_t(blobMeshlets.size()) * sizeof(Meshlet);
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

    PackHeader hdr{};
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "ISKURPK", 7);
    hdr.version = 2;
    hdr.primCount = uint32_t(prims.size());

    const uint64_t ofsHeader = 0;
    const uint64_t ofsPrimTbl = ofsHeader + sizeof(PackHeader);
    const uint64_t ofsVertices = ofsPrimTbl + uint64_t(prims.size()) * sizeof(PrimRecord);
    const uint64_t ofsIndices = ofsVertices + uint64_t(blobVertices.size()) * sizeof(Vertex);
    const uint64_t ofsMeshlets = ofsIndices + uint64_t(blobIndices.size()) * sizeof(uint32_t);
    const uint64_t ofsMLVerts = ofsMeshlets + uint64_t(blobMeshlets.size()) * sizeof(Meshlet);
    const uint64_t ofsMLTris = ofsMLVerts + uint64_t(blobMLVerts.size()) * sizeof(uint32_t);
    const uint64_t ofsMLBounds = ofsMLTris + uint64_t(blobMLTris.size());

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
        std::fprintf(stderr, "Failed to open pack for write: %s\n", outPackPath.string().c_str());
        return false;
    }

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(prims.data()), prims.size() * sizeof(PrimRecord));
    out.write(reinterpret_cast<const char*>(blobVertices.data()), blobVertices.size() * sizeof(Vertex));
    out.write(reinterpret_cast<const char*>(blobIndices.data()), blobIndices.size() * sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(blobMeshlets.data()), blobMeshlets.size() * sizeof(Meshlet));
    out.write(reinterpret_cast<const char*>(blobMLVerts.data()), blobMLVerts.size() * sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(blobMLTris.data()), blobMLTris.size());
    out.write(reinterpret_cast<const char*>(blobMLBounds.data()), blobMLBounds.size() * sizeof(MeshletBounds));

    if (!out.good())
    {
        std::fprintf(stderr, "Write error while saving pack: %s\n", outPackPath.string().c_str());
        return false;
    }

    std::printf("Meshes pack written: %s\n", outPackPath.string().c_str());
    std::printf("  prims=%zu, verts=%zu, inds=%zu, meshlets=%zu, mlVerts=%zu, mlTris=%zu bytes, mlBounds=%zu\n", prims.size(), blobVertices.size(), blobIndices.size(), blobMeshlets.size(),
                blobMLVerts.size(), blobMLTris.size(), blobMLBounds.size());
    return true;
}

static void PrintUsage()
{
    std::puts("IskurScenePacker - GLB → textures(.dds) + meshes(.iskurpack)\n"
              "Usage:\n"
              "  IskurScenePacker --input <scene-name> [--textures-only | --meshes-only | --skip-textures | --skip-meshes] [--fast] [--output <override.iskurpack>]\n"
              "  IskurScenePacker --all [--textures-only | --meshes-only | --skip-textures | --skip-meshes] [--fast]\n"
              "Notes:\n"
              "  - Single scene mode resolves GLB as data/scenes/<scene>/<scene>.glb.\n"
              "  - Single scene output defaults to data/scenes/<scene>/<scene>.iskurpack (use --output to override).\n"
              "  - --all processes every directory under data/scenes that contains <name>/<name>.glb.\n"
              "    (Per-scene output is <name>/<name>.iskurpack; --output is ignored in --all mode.)\n"
              "  - Textures go to <scene>/textures/<imageIndex>.dds (BC7 for color/emissive, BC5 for normals).\n"
              "  - --fast enables BC7 quick mode and parallel per-image processing.\n");
}

int WriteIskurScene(const std::filesystem::path& inGlb, const std::filesystem::path& outPack, const RunOptions& opt)
{
    if (!IsGlbFile(inGlb))
    {
        std::fprintf(stderr, "Input is not a GLB (missing 'glTF' magic): %s\n", inGlb.string().c_str());
        return 3;
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string warn, err;

    bool ok = loader.LoadBinaryFromFile(&model, &warn, &err, inGlb.string());
    if (!warn.empty())
    {
        std::fprintf(stderr, "[tinygltf] warn: %s\n", warn.c_str());
    }
    if (!ok)
    {
        if (!err.empty())
        {
            std::fprintf(stderr, "[tinygltf] err : %s\n", err.c_str());
        }
        return 3;
    }

    std::fprintf(stdout, "Loaded GLB: %s\n", inGlb.string().c_str());

    const auto imgUsage = BuildImageUsageFlags(model);

    bool texOK = true;
    if (opt.doTextures)
    {
        texOK = ProcessAllTextures(inGlb, model, imgUsage, opt);
    }

    bool meshOK = true;
    if (opt.doMeshes)
    {
        meshOK = ProcessAllMeshesAndWritePack(outPack, model);
    }

    return (texOK && meshOK) ? 0 : 4;
}

int main(int argc, char** argv)
{
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needCoUninit = SUCCEEDED(cohr);
    if (cohr == RPC_E_CHANGED_MODE)
    {
        needCoUninit = false;
    }
    else if (FAILED(cohr))
    {
        std::fprintf(stderr, "COM init failed: 0x%08X\n", (unsigned)cohr);
        return 1;
    }

    std::filesystem::path inSceneName;
    std::filesystem::path outPack;
    bool processAll = false;

    RunOptions opt;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if ((a == "-i" || a == "--input") && i + 1 < argc)
        {
            inSceneName = argv[++i];
        }
        else if ((a == "-o" || a == "--output") && i + 1 < argc)
        {
            outPack = argv[++i];
        }
        else if (a == "--textures-only")
        {
            opt.doTextures = true;
            opt.doMeshes = false;
        }
        else if (a == "--meshes-only")
        {
            opt.doTextures = false;
            opt.doMeshes = true;
        }
        else if (a == "--skip-textures")
        {
            opt.doTextures = false;
        }
        else if (a == "--skip-meshes")
        {
            opt.doMeshes = false;
        }
        else if (a == "--fast")
        {
            opt.fast = true;
        }
        else if (a == "--all" || a == "--all-scenes")
        {
            processAll = true;
        }
        else
        {
            std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            PrintUsage();
            if (needCoUninit)
            {
                CoUninitialize();
            }
            return 2;
        }
    }

    // ---------------- All-scenes mode ----------------
    if (processAll)
    {
        if (!outPack.empty())
        {
            std::fprintf(stdout, "[warn] --output is ignored in --all mode (one pack per scene).\n");
        }

        const std::filesystem::path scenesRoot = std::filesystem::path("data") / "scenes";
        if (!(std::filesystem::exists(scenesRoot) && std::filesystem::is_directory(scenesRoot)))
        {
            std::fprintf(stderr, "Scenes root not found or not a directory: %s\n", scenesRoot.string().c_str());
            if (needCoUninit)
            {
                CoUninitialize();
            }
            return 1;
        }

        int total = 0, ok = 0, failed = 0, skipped = 0;

        for (const auto& de : std::filesystem::directory_iterator(scenesRoot))
        {
            if (!de.is_directory())
            {
                continue;
            }
            const std::string name = de.path().filename().string();
            const std::filesystem::path glb = de.path() / (name + ".glb");
            const std::filesystem::path pack = de.path() / (name + ".iskurpack");

            if (!(std::filesystem::exists(glb) && std::filesystem::is_regular_file(glb)))
            {
                std::fprintf(stdout, "[skip] %s (missing %s)\n", name.c_str(), glb.string().c_str());
                ++skipped;
                continue;
            }
            if (!IsGlbFile(glb))
            {
                std::fprintf(stdout, "[skip] %s (not a GLB: %s)\n", name.c_str(), glb.string().c_str());
                ++skipped;
                continue;
            }

            std::fprintf(stdout, "=== Processing scene: %s ===\n", name.c_str());
            std::fflush(stdout);

            ++total;
            const int rc = WriteIskurScene(glb, pack, opt);
            if (rc == 0)
            {
                ++ok;
            }
            else
            {
                ++failed;
            }
        }

        std::fprintf(stdout, "All-scenes summary: total=%d, ok=%d, failed=%d, skipped=%d (fast=%s)\n", total, ok, failed, skipped, opt.fast ? "yes" : "no");

        if (needCoUninit)
        {
            CoUninitialize();
        }
        return (failed == 0) ? 0 : 4;
    }

    // ---------------- Single-scene mode (existing behavior) ----------------
    if (inSceneName.empty())
    {
        PrintUsage();
        if (needCoUninit)
        {
            CoUninitialize();
        }
        return 1;
    }

    const std::string name = inSceneName.string();
    const std::filesystem::path sceneDir = std::filesystem::path("data") / "scenes" / name;
    const std::filesystem::path resolved = sceneDir / (name + ".glb");

    if (outPack.empty())
    {
        outPack = sceneDir / (name + ".iskurpack");
    }

    if (!(std::filesystem::exists(resolved) && std::filesystem::is_regular_file(resolved)))
    {
        std::fprintf(stderr, "Could not find GLB: %s\n", resolved.string().c_str());
        if (needCoUninit)
        {
            CoUninitialize();
        }
        return 1;
    }

    if (!IsGlbFile(resolved))
    {
        std::fprintf(stderr, "Input is not a GLB file: %s\n", resolved.string().c_str());
        if (needCoUninit)
        {
            CoUninitialize();
        }
        return 1;
    }

    int rc = WriteIskurScene(resolved, outPack, opt);

    if (needCoUninit)
    {
        CoUninitialize();
    }
    return rc;
}