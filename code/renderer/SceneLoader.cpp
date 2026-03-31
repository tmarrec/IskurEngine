// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneLoader.h"

#include "SceneFileLoader.h"
#include "SceneUtils.h"
#include "common/IskurPackFormat.h"

namespace
{
bool IsFiniteFloat3(const XMFLOAT3& v)
{
    return IE_IsFinite(v.x) && IE_IsFinite(v.y) && IE_IsFinite(v.z);
}

bool IsFiniteFloat4x4(const XMFLOAT4X4& m)
{
    return IE_IsFinite(m._11) && IE_IsFinite(m._12) && IE_IsFinite(m._13) && IE_IsFinite(m._14) && IE_IsFinite(m._21) && IE_IsFinite(m._22) && IE_IsFinite(m._23) && IE_IsFinite(m._24) &&
           IE_IsFinite(m._31) && IE_IsFinite(m._32) && IE_IsFinite(m._33) && IE_IsFinite(m._34) && IE_IsFinite(m._41) && IE_IsFinite(m._42) && IE_IsFinite(m._43) && IE_IsFinite(m._44);
}
} // namespace

LoadedScene SceneLoader::Load(const String& sceneFile)
{
    const std::filesystem::path packPath = SceneUtils::ResolveScenePackPath(sceneFile);
    LoadedScene scene{};
    scene.sourceData = LoadSceneFile(packPath);
    const SceneFileData& sceneData = scene.sourceData;
    LoadTextures(scene, sceneData);
    LoadSamplers(scene, sceneData);
    LoadMaterials(scene, sceneData);
    BuildPrimitives(scene, sceneData);
    BuildInstances(scene, sceneData);
    return scene;
}

void SceneLoader::LoadTextures(LoadedScene& outScene, const SceneFileData& scene)
{
    outScene.textures.clear();

    const auto& texTable = scene.texTable;
    if (texTable.empty())
    {
        return;
    }

    IE_Assert(scene.TexBlob() != nullptr);
    IE_Assert(scene.texBlobSize > 0);
    const u8* texBlob = scene.TexBlob();

    outScene.textures.resize(texTable.size());
    for (u32 i = 0; i < texTable.size(); ++i)
    {
        const auto& tr = texTable[i];
        IE_Assert(static_cast<size_t>(tr.byteOffset) + static_cast<size_t>(tr.byteSize) <= static_cast<size_t>(scene.texBlobSize));
        IE_Assert(static_cast<size_t>(tr.subresourceOffset) + static_cast<size_t>(tr.subresourceCount) <= static_cast<size_t>(scene.texSubresources.size()));

        LoadedTexture texture{};
        texture.format = tr.format;
        texture.dimension = tr.dimension;
        texture.miscFlags = tr.miscFlags;
        texture.miscFlags2 = tr.miscFlags2;
        texture.width = tr.width;
        texture.height = tr.height;
        texture.depth = tr.depth;
        texture.arraySize = tr.arraySize;
        texture.mipLevels = tr.mipLevels;
        texture.subresources = scene.texSubresources.data() + tr.subresourceOffset;
        texture.subresourceCount = tr.subresourceCount;
        texture.texelBytes = texBlob + static_cast<size_t>(tr.byteOffset);
        texture.texelByteCount = static_cast<size_t>(tr.byteSize);
        outScene.textures[i] = std::move(texture);
    }
}

void SceneLoader::LoadSamplers(LoadedScene& outScene, const SceneFileData& scene)
{
    outScene.samplers = scene.samplers;
}

void SceneLoader::LoadMaterials(LoadedScene& outScene, const SceneFileData& scene)
{
    outScene.materials.clear();

    const auto& matlTable = scene.materials;
    if (matlTable.empty())
    {
        LoadedMaterial fallback{};
        fallback.metallicFactor = 0.0f;
        fallback.roughnessFactor = 1.0f;
        fallback.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
        fallback.alphaMode = AlphaMode_Opaque;
        fallback.alphaCutoff = 0.5f;
        fallback.normalScale = 1.0f;
        fallback.baseColorTextureIndex = -1;
        fallback.baseColorSamplerIndex = -1;
        fallback.metallicRoughnessTextureIndex = -1;
        fallback.metallicRoughnessSamplerIndex = -1;
        fallback.normalTextureIndex = -1;
        fallback.normalSamplerIndex = -1;
        fallback.aoTextureIndex = -1;
        fallback.aoSamplerIndex = -1;
        fallback.emissiveTextureIndex = -1;
        fallback.emissiveSamplerIndex = -1;
        fallback.emissiveFactor = {0.0f, 0.0f, 0.0f};
        fallback.doubleSided = 0;
        outScene.materials.push_back(fallback);
        return;
    }

    outScene.materials.resize(matlTable.size());
    for (u32 i = 0; i < matlTable.size(); ++i)
    {
        const auto& mr = matlTable[i];
        LoadedMaterial m{};
        m.baseColorFactor = {mr.baseColorFactor[0], mr.baseColorFactor[1], mr.baseColorFactor[2], mr.baseColorFactor[3]};
        m.metallicFactor = mr.metallicFactor;
        m.roughnessFactor = mr.roughnessFactor;
        m.normalScale = mr.normalScale;
        m.alphaCutoff = mr.alphaCutoff;
        m.alphaMode = (mr.flags & IEPack::MATF_ALPHA_BLEND || mr.flags & IEPack::MATF_ALPHA_MASK) ? AlphaMode_Mask : AlphaMode_Opaque;
        m.doubleSided = (mr.flags & IEPack::MATF_DOUBLE_SIDED) ? 1 : 0;
        m.emissiveFactor = {mr.emissiveFactor[0], mr.emissiveFactor[1], mr.emissiveFactor[2]};

        auto mapTex = [&](int txhdIdx) -> i32 {
            if (txhdIdx < 0)
            {
                return -1;
            }
            IE_Assert(static_cast<size_t>(txhdIdx) < outScene.textures.size());
            return txhdIdx;
        };
        auto mapSamp = [&](u32 sampIdx, int txhdIdx) -> i32 {
            if (txhdIdx < 0 || sampIdx == UINT32_MAX)
            {
                return -1;
            }
            IE_Assert(static_cast<size_t>(sampIdx) < outScene.samplers.size());
            return static_cast<i32>(sampIdx);
        };

        m.baseColorTextureIndex = mapTex(mr.baseColorTx);
        m.baseColorSamplerIndex = mapSamp(mr.baseColorSampler, mr.baseColorTx);
        m.metallicRoughnessTextureIndex = mapTex(mr.metallicRoughTx);
        m.metallicRoughnessSamplerIndex = mapSamp(mr.metallicRoughSampler, mr.metallicRoughTx);
        m.normalTextureIndex = mapTex(mr.normalTx);
        m.normalSamplerIndex = mapSamp(mr.normalSampler, mr.normalTx);
        m.aoTextureIndex = mapTex(mr.occlusionTx);
        m.aoSamplerIndex = mapSamp(mr.occlusionSampler, mr.occlusionTx);
        m.emissiveTextureIndex = mapTex(mr.emissiveTx);
        m.emissiveSamplerIndex = mapSamp(mr.emissiveSampler, mr.emissiveTx);

        outScene.materials[i] = m;
    }
}

void SceneLoader::BuildPrimitives(LoadedScene& outScene, const SceneFileData& scene)
{
    outScene.primitives.clear();

    const auto& prims = scene.prims;
    if (prims.empty())
    {
        return;
    }

    IE_Assert(scene.VertBlob() && scene.IdxBlob() && scene.MshlBlob() && scene.MlvtBlob() && scene.MltrBlob() && scene.MlbdBlob());
    const u8* vertBase = scene.VertBlob();
    const u8* idxBase = scene.IdxBlob();
    const u8* mshlBase = scene.MshlBlob();
    const u8* mlvtBase = scene.MlvtBlob();
    const u8* mltrBase = scene.MltrBlob();
    const u8* mlbdBase = scene.MlbdBlob();

    outScene.primitives.reserve(prims.size());
    for (const IEPack::PrimRecord& r : prims)
    {
        const auto* vtx = reinterpret_cast<const Vertex*>(vertBase + r.vertexByteOffset);
        const auto* idx = reinterpret_cast<const u32*>(idxBase + r.indexByteOffset);
        const auto* mlt = reinterpret_cast<const Meshlet*>(mshlBase + r.meshletsByteOffset);
        const auto* mlv = reinterpret_cast<const u32*>(mlvtBase + r.mlVertsByteOffset);
        const auto* mltb = mltrBase + r.mlTrisByteOffset;
        const auto* mlb = reinterpret_cast<const MeshletBounds*>(mlbdBase + r.mlBoundsByteOffset);

        LoadedPrimitive prim{};
        prim.vertices = vtx;
        prim.vertexCount = r.vertexCount;
        prim.indices = idx;
        prim.indexCount = r.indexCount;
        prim.meshlets = mlt;
        prim.meshletVertices = mlv;
        prim.meshletVertexCount = r.mlVertsCount;
        prim.meshletTriangles = mltb;
        prim.meshletTriangleByteCount = r.mlTrisByteCount;
        prim.meshletBounds = mlb;
        prim.meshletCount = r.meshletCount;
        prim.localBoundsCenter = r.localBoundsCenter;
        prim.localBoundsRadius = r.localBoundsRadius;
        IE_Assert(IsFiniteFloat3(prim.localBoundsCenter));
        IE_Assert(IE_IsFinite(prim.localBoundsRadius) && prim.localBoundsRadius >= 0.0f);

        outScene.primitives.push_back(std::move(prim));
    }
}

void SceneLoader::BuildInstances(LoadedScene& outScene, const SceneFileData& scene)
{
    outScene.instances.clear();
    outScene.instances.reserve(scene.instances.size());

    for (const IEPack::InstanceRecord& inst : scene.instances)
    {
        IE_Assert(inst.primIndex < outScene.primitives.size());
        IE_Assert(inst.materialIndex < outScene.materials.size());
        IE_Assert(IsFiniteFloat4x4(inst.world));

        InstanceData instance{};
        instance.primIndex = inst.primIndex;
        instance.materialIndex = inst.materialIndex;
        instance.world = inst.world;
        outScene.instances.push_back(instance);
    }
}
