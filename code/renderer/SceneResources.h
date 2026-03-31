// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Buffer.h"
#include "Primitive.h"
#include "Raytracing.h"
#include "RenderSceneTypes.h"
#include "Texture.h"
#include "common/Types.h"
#include "shaders/CPUGPU.h"

class BindlessHeaps;
class RenderDevice;
struct LoadedScene;

class SceneResources
{
  public:
    SceneResources(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps);

    void Reset();
    void ImportScene(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void RefreshAvailableScenes();
    void RequestSceneSwitch(const String& sceneFile);

    String ResolveSceneName(const String& sceneFile) const;

    bool HasPendingSceneSwitch() const;
    const String& GetCurrentSceneFile() const;
    const String& GetPendingSceneFile() const;
    const Vector<String>& GetAvailableScenes() const;

    void SetCurrentSceneFile(const String& sceneFile);
    void ClearPendingSceneSwitch();

    const SharedPtr<Buffer>& GetMaterialsBuffer() const;
    const Vector<Material>& GetMaterials() const;
    const Vector<Primitive>& GetPrimitives() const;
    Vector<Primitive>& GetPrimitives();
    const Vector<InstanceData>& GetInstances() const;
    Vector<InstanceData>& GetInstances();
    Vector<Raytracing::RTInstance> BuildRTInstances() const;

    u32 GetLinearSamplerIdx() const;

    bool AreInstancesDirty() const;
    void MarkInstancesDirty();
    void ClearInstancesDirty();

  private:
    void ImportSceneTextures(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void ImportSceneSamplers(const LoadedScene& scene);
    void CreateDefaultSamplers();
    void ImportSceneMaterials(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void ImportScenePrimitives(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void ImportSceneInstances(const LoadedScene& scene);

    RenderDevice& m_RenderDevice;
    BindlessHeaps& m_BindlessHeaps;

    SharedPtr<Buffer> m_MaterialsBuffer;
    Vector<Material> m_Materials;

    u32 m_LinearSamplerIdx = UINT_MAX;
    u32 m_DefaultWrapSamplerIdx = UINT_MAX;

    Vector<u32> m_TxhdToSrv;
    Vector<u32> m_SampToHeap;

    Vector<Primitive> m_Primitives;
    Vector<InstanceData> m_Instances;
    bool m_InstancesDirty = true;

    Vector<String> m_AvailableScenes;
    String m_CurrentSceneFile;
    String m_PendingSceneFile;

    Vector<Texture> m_Textures;
};
