// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Mesh.h"

#include "Node.h"

Mesh::Mesh(const SharedPtr<Node>& parentNode)
{
    m_ParentNode = parentNode;
}

void Mesh::SetPrimitives(const tinygltf::Mesh& mesh)
{
    for (const tinygltf::Primitive& gltfPrimitive : mesh.primitives)
    {
        SharedPtr<Primitive> primitive = IE_MakeSharedPtr<Primitive>(gltfPrimitive, shared_from_this());
        m_Primitives.Add(primitive);
    }
}

const Vector<SharedPtr<Primitive>>& Mesh::GetPrimitives()
{
    return m_Primitives;
}

float4x4 Mesh::GetTransform() const
{
    float4x4 transform = float4x4::Identity();
    SharedPtr<Node> parentNode = m_ParentNode;
    while (parentNode)
    {
        transform = transform * parentNode->localTransform;
        parentNode = parentNode->parentNode;
    }
    return transform;
}
