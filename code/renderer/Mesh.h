// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <tiny_gltf.h>

#include "Primitive.h"

struct Node;

class Mesh : public std::enable_shared_from_this<Mesh>
{
  public:
    Mesh(const SharedPtr<Node>& parentNode, i32 meshIndex);
    void SetPrimitives(const tinygltf::Mesh& mesh);
    const Vector<SharedPtr<Primitive>>& GetPrimitives();
    float4x4 GetTransform() const;

  private:
    Vector<SharedPtr<Primitive>> m_Primitives;
    WeakPtr<Node> m_ParentNode;
    i32 m_Index;
};