// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <tiny_gltf.h>

#include "../common/Types.h"
#include "Scene.h"

class World
{
  public:
    World(const tinygltf::Model& model, const String& sceneFilename);
    Vector<SharedPtr<Primitive>> GetPrimitives();

  private:
    static void InitNodes(const tinygltf::Model& model, Scene& scene, i32 nodeIndex, const SharedPtr<Node>& parentNode);

    Vector<Scene> m_Scenes;
    u32 m_CurrentSceneIdx = UINT_MAX;

    u32 m_CurrentCameraIdx = UINT_MAX;
    Vector<tinygltf::Camera> m_Cameras;
};