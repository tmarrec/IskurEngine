// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Mesh.h"

#include "Node.h"

Mesh::Mesh(const SharedPtr<Node>& parentNode, i32 meshIndex)
{
    m_ParentNode = parentNode;
    m_Index = meshIndex;
}

void Mesh::SetPrimitives(const tinygltf::Mesh& mesh)
{
    for (u32 i = 0; i < mesh.primitives.size(); ++i)
    {
        const tinygltf::Primitive& gltfPrimitive = mesh.primitives[i];
        SharedPtr<Primitive> primitive = IE_MakeSharedPtr<Primitive>(gltfPrimitive, shared_from_this(), m_Index, i);
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
    SharedPtr<Node> parentNode = m_ParentNode.lock();
    while (parentNode)
    {
        transform = parentNode->localTransform * transform;
        parentNode = parentNode->parentNode.lock();
    }
    return transform;
}
