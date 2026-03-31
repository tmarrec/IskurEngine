// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "SceneFileLoader.h"
#include "RenderSceneTypes.h"
#include "common/Types.h"
#include "shaders/CPUGPU.h"

struct LoadedTexture
{
    u32 format = 0;
    u32 dimension = 0;
    u32 miscFlags = 0;
    u32 miscFlags2 = 0;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
    u32 arraySize = 0;
    u32 mipLevels = 0;
    const IEPack::TextureSubresourceRecord* subresources = nullptr;
    u32 subresourceCount = 0;
    const u8* texelBytes = nullptr;
    size_t texelByteCount = 0;
};

struct LoadedMaterial
{
    f32 metallicFactor = 0.0f;
    f32 roughnessFactor = 1.0f;
    i32 baseColorTextureIndex = -1;
    i32 baseColorSamplerIndex = -1;

    XMFLOAT4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};

    u32 alphaMode = AlphaMode_Opaque;
    f32 alphaCutoff = 0.5f;
    i32 metallicRoughnessTextureIndex = -1;
    i32 metallicRoughnessSamplerIndex = -1;

    i32 normalTextureIndex = -1;
    i32 normalSamplerIndex = -1;
    f32 normalScale = 1.0f;
    i32 doubleSided = 0;

    i32 aoTextureIndex = -1;
    i32 aoSamplerIndex = -1;
    i32 emissiveTextureIndex = -1;
    i32 emissiveSamplerIndex = -1;
    XMFLOAT3 emissiveFactor = {0.0f, 0.0f, 0.0f};
};

struct LoadedPrimitive
{
    const Vertex* vertices = nullptr;
    u32 vertexCount = 0;
    const u32* indices = nullptr;
    u32 indexCount = 0;
    const Meshlet* meshlets = nullptr;
    const u32* meshletVertices = nullptr;
    u32 meshletVertexCount = 0;
    const u8* meshletTriangles = nullptr;
    u32 meshletTriangleByteCount = 0;
    const MeshletBounds* meshletBounds = nullptr;

    u32 meshletCount = 0;
    XMFLOAT3 localBoundsCenter = {0.0f, 0.0f, 0.0f};
    f32 localBoundsRadius = 0.0f;
};

struct LoadedScene
{
    LoadedScene() = default;
    LoadedScene(const LoadedScene&) = delete;
    LoadedScene& operator=(const LoadedScene&) = delete;
    LoadedScene(LoadedScene&&) noexcept = default;
    LoadedScene& operator=(LoadedScene&&) noexcept = default;

    // Owns the pack bytes backing the texture and primitive views below.
    SceneFileData sourceData;
    Vector<LoadedTexture> textures;
    Vector<D3D12_SAMPLER_DESC> samplers;
    Vector<LoadedMaterial> materials;
    Vector<LoadedPrimitive> primitives;
    Vector<InstanceData> instances;
};

class SceneLoader
{
  public:
    static LoadedScene Load(const String& sceneFile);

  private:
    static void LoadTextures(LoadedScene& outScene, const SceneFileData& scene);
    static void LoadSamplers(LoadedScene& outScene, const SceneFileData& scene);
    static void LoadMaterials(LoadedScene& outScene, const SceneFileData& scene);
    static void BuildPrimitives(LoadedScene& outScene, const SceneFileData& scene);
    static void BuildInstances(LoadedScene& outScene, const SceneFileData& scene);
};

