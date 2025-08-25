// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Primitive.h"

#include <DirectXMesh.h>

#include "../common/Asserts.h"
#include "Mesh.h"
#include "ScenePack.h"

Primitive::Primitive(const tinygltf::Primitive& gltfPrimitive, const SharedPtr<Mesh>& parentMesh, i32 meshIndex, i32 primIndex)
{
    m_GltfPrimitive = gltfPrimitive;
    m_ParentMesh = parentMesh;
    m_MeshIndex = meshIndex;
    m_PrimIndex = primIndex;
}

void Primitive::Process()
{
    const PackedPrimitiveView* view = ScenePack::Get().FindPrimitive(m_MeshIndex, m_PrimIndex);
    IE_Assert(view && "Packed primitive not found");

    m_MaterialIdx = view->materialIndex;

    m_Vertices.Resize(view->vertexCount);
    memcpy(m_Vertices.Data(), view->vertices, sizeof(Vertex) * view->vertexCount);
    m_Indices.Resize(view->indexCount);
    memcpy(m_Indices.Data(), view->indices, sizeof(u32) * view->indexCount);
    m_Meshlets.Resize(view->meshletCount);
    memcpy(m_Meshlets.Data(), view->meshlets, sizeof(Meshlet) * view->meshletCount);
    m_MeshletVertices.Resize(view->mlVertCount);
    memcpy(m_MeshletVertices.Data(), view->mlVerts, sizeof(u32) * view->mlVertCount);
    m_MeshletTriangles.Resize(view->mlTriCountBytes);
    memcpy(m_MeshletTriangles.Data(), view->mlTris, view->mlTriCountBytes);
    m_MeshletBounds.Resize(view->mlBoundsCount);
    memcpy(m_MeshletBounds.Data(), view->mlBounds, sizeof(MeshletBounds) * view->mlBoundsCount);
}

float4x4 Primitive::GetTransform() const
{
    if (SharedPtr<Mesh> parentMesh = m_ParentMesh.lock())
    {
        return parentMesh->GetTransform();
    }
    return float4x4::Identity();
}
