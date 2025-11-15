// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Renderer.h"
#include "SceneFileLoader.h"
#include "common/Types.h"

class SceneLoader
{
  public:
    static void Load(Renderer& renderer, const String& sceneFile);

  private:
    static void LoadTextures(Renderer& renderer, const SceneFileData& scene);
    static void LoadSamplers(Renderer& renderer, const SceneFileData& scene);
    static void LoadMaterials(Renderer& renderer, const SceneFileData& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    static void BuildPrimitives(Renderer& renderer, const SceneFileData& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd);

    static void SetupDepthResourcesAndLinearSampler(Renderer& renderer);
    static void SubmitAndSync(Renderer& renderer, Renderer::PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
};
