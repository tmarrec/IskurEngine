// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Primitive.h"

#include <DirectXMesh.h>
#include <meshoptimizer.h>

#include "../common/Asserts.h"
#include "Mesh.h"

Primitive::Primitive(const tinygltf::Primitive& gltfPrimitive, const SharedPtr<Mesh>& parentMesh)
{
    m_GltfPrimitive = gltfPrimitive;
    m_ParentMesh = parentMesh;
}

void Primitive::Process(const tinygltf::Model& model, const String& sceneFilename)
{
    Vector<Vertex> initialVertices;
    Vector<u32> initialIndices;

    Vector<float3> normals;
    Vector<float2> texCoords;
    Vector<float4> tangents;

    if (m_GltfPrimitive.mode != TINYGLTF_MODE_TRIANGLES)
    {
        IE_Assert(false); // not implemented!
    }

    m_MaterialIdx = m_GltfPrimitive.material;
    if (m_MaterialIdx == UINT_MAX)
    {
        m_MaterialIdx = 0;
    }

    if (m_GltfPrimitive.attributes.contains("NORMAL"))
    {
        const tinygltf::Accessor& accessor = model.accessors[m_GltfPrimitive.attributes.at("NORMAL")];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        const u8* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
        const u32 count = static_cast<u32>(accessor.count);
        const u32 stride = accessor.ByteStride(bufferView) ? accessor.ByteStride(bufferView) : tinygltf::GetComponentSizeInBytes(accessor.type) * tinygltf::GetNumComponentsInType(accessor.type);

        normals.Resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            normals[i] = *reinterpret_cast<const float3*>(data + i * stride);
        }
    }

    if (m_GltfPrimitive.attributes.contains("TEXCOORD_0"))
    {
        const tinygltf::Accessor& accessor = model.accessors[m_GltfPrimitive.attributes.at("TEXCOORD_0")];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        const u8* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
        const u32 count = static_cast<u32>(accessor.count);
        const u32 stride = accessor.ByteStride(bufferView) ? accessor.ByteStride(bufferView) : tinygltf::GetComponentSizeInBytes(accessor.type) * tinygltf::GetNumComponentsInType(accessor.type);

        texCoords.Resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            texCoords[i] = *reinterpret_cast<const float2*>(data + i * stride);
        }
    }

    if (m_GltfPrimitive.attributes.contains("TANGENT"))
    {
        const tinygltf::Accessor& accessor = model.accessors[m_GltfPrimitive.attributes.at("TANGENT")];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        const u8* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
        const u32 count = static_cast<u32>(accessor.count);
        const u32 stride = accessor.ByteStride(bufferView) ? accessor.ByteStride(bufferView) : tinygltf::GetComponentSizeInBytes(accessor.type) * tinygltf::GetNumComponentsInType(accessor.type);

        tangents.Resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            tangents[i] = *reinterpret_cast<const float4*>(data + i * stride);
        }
    }

    if (m_GltfPrimitive.attributes.contains("POSITION"))
    {
        const tinygltf::Accessor& accessor = model.accessors[m_GltfPrimitive.attributes.at("POSITION")];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        const u8* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
        const u32 count = static_cast<u32>(accessor.count);
        const u32 stride = accessor.ByteStride(bufferView) ? accessor.ByteStride(bufferView) : tinygltf::GetComponentSizeInBytes(accessor.type) * tinygltf::GetNumComponentsInType(accessor.type);

        for (u32 i = 0; i < count; ++i)
        {
            Vertex v;
            v.position = *reinterpret_cast<const float3*>(data + i * stride);
            if (!normals.Empty())
            {
                v.normal = normals[i];
            }
            else
            {
                v.normal = {0, 0, 0};
            }
            if (!texCoords.Empty())
            {
                v.texCoord = texCoords[i];
            }
            else
            {
                v.texCoord = {0, 0};
            }
            if (!tangents.Empty())
            {
                v.tangent = tangents[i];
            }
            else
            {
                v.tangent = {0, 0, 0, 0};
            }
            initialVertices.Add(v);
        }
    }

    // === Extract indices ===
    if (m_GltfPrimitive.indices >= 0)
    {
        const tinygltf::Accessor& accessor = model.accessors[m_GltfPrimitive.indices];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        const u8* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
        const u32 count = static_cast<u32>(accessor.count);

        initialIndices.Resize(count);
        for (u32 i = 0; i < initialIndices.Size(); ++i)
        {
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            {
                initialIndices[i] = static_cast<uint32_t>(data[i]);
            }
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                initialIndices[i] = static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(data)[i]);
            }
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            {
                initialIndices[i] = static_cast<uint32_t>(reinterpret_cast<const uint32_t*>(data)[i]);
            }
            else
            {
                IE_Assert(false);
            }
        }
    }
    else
    {
        // No indices - generate sequential indices
        initialIndices.Resize(initialVertices.Size());
        for (u32 i = 0; i < initialVertices.Size(); ++i)
        {
            initialIndices[i] = i;
        }
    }

    if (normals.Empty())
    {
        for (u32 i = 0; i < initialIndices.Size(); i += 3)
        {
            const u32 idx0 = initialIndices[i];
            const u32 idx1 = initialIndices[i + 1];
            const u32 idx2 = initialIndices[i + 2];
            const float3& v0 = initialVertices[idx0].position;
            const float3& v1 = initialVertices[idx1].position;
            const float3& v2 = initialVertices[idx2].position;
            const float3 edge1 = v1 - v0;
            const float3 edge2 = v2 - v0;
            const float3 faceNormal = float3::Cross(edge1, edge2).Normalized();
            initialVertices[idx0].normal = faceNormal;
            initialVertices[idx1].normal = faceNormal;
            initialVertices[idx2].normal = faceNormal;
        }
    }

    if (tangents.Empty() && !texCoords.Empty() && !normals.Empty())
    {
        u32 faceCount = initialIndices.Size() / 3;
        u32 vertCount = initialVertices.Size();

        Vector<DirectX::XMFLOAT3> positions(vertCount);
        Vector<DirectX::XMFLOAT3> normalsDx(vertCount);
        Vector<DirectX::XMFLOAT2> uvs(vertCount);
        Vector<DirectX::XMFLOAT4> outTangents(vertCount);

        for (u32 i = 0; i < vertCount; ++i)
        {
            const Vertex& v = initialVertices[i];
            positions[i] = {v.position.x, v.position.y, v.position.z};
            normalsDx[i] = {v.normal.x, v.normal.y, v.normal.z};
            uvs[i] = {v.texCoord.x, v.texCoord.y};
        }

        IE_Check(DirectX::ComputeTangentFrame(initialIndices.Data(), faceCount, positions.Data(), normalsDx.Data(), uvs.Data(), vertCount, outTangents.Data()));

        for (u32 i = 0; i < vertCount; ++i)
        {
            initialVertices[i].tangent = {outTangents[i].x, outTangents[i].y, outTangents[i].z, -outTangents[i].w};
        }
    }

    // Load vertices/indices
    Vector<u32> remap(initialIndices.Size());
    const u32 totalVertices = static_cast<u32>(meshopt_generateVertexRemap(remap.Data(), initialIndices.Data(), initialIndices.Size(), initialVertices.Data(), initialVertices.Size(), sizeof(Vertex)));
    m_Indices.Resize(initialIndices.Size());
    meshopt_remapIndexBuffer(m_Indices.Data(), initialIndices.Data(), initialIndices.Size(), remap.Data());
    m_Vertices.Resize(totalVertices);
    meshopt_remapVertexBuffer(m_Vertices.Data(), initialVertices.Data(), initialVertices.Size(), sizeof(Vertex), remap.Data());

    // Optimize mesh
    meshopt_optimizeVertexCache(m_Indices.Data(), m_Indices.Data(), m_Indices.Size(), m_Vertices.Size());
    meshopt_optimizeOverdraw(m_Indices.Data(), m_Indices.Data(), m_Indices.Size(), &m_Vertices[0].position.x, m_Vertices.Size(), sizeof(Vertex), 1.05f);
    meshopt_optimizeVertexFetch(m_Vertices.Data(), m_Indices.Data(), m_Indices.Size(), m_Vertices.Data(), m_Vertices.Size(), sizeof(Vertex));

    // Generate meshlets data
    constexpr u64 maxVertices = 64;
    constexpr u64 maxTriangles = 124; // NVidia-recommended 126, rounded down to a multiple of 4
    constexpr f32 coneWeight = 0.f;   // note: should be set to 0 unless cone culling is used at runtime!

    const u32 maxMeshlets = static_cast<u32>(meshopt_buildMeshletsBound(m_Indices.Size(), maxVertices, maxTriangles));
    m_Meshlets.Resize(maxMeshlets);
    m_MeshletVertices.Resize(maxMeshlets * maxVertices);
    m_MeshletTriangles.Resize(maxMeshlets * maxTriangles * 3);
    const u32 meshletsCount = static_cast<u32>(meshopt_buildMeshlets(m_Meshlets.Data(), m_MeshletVertices.Data(), m_MeshletTriangles.Data(), m_Indices.Data(), m_Indices.Size(),
                                                                     &m_Vertices[0].position.x, m_Vertices.Size(), sizeof(Vertex), maxVertices, maxTriangles, coneWeight));
    m_Meshlets.Resize(meshletsCount);
    if (!m_Meshlets.Empty())
    {
        const meshopt_Meshlet& last = m_Meshlets.Back();
        m_MeshletVertices.Resize(last.vertex_offset + last.vertex_count);
        m_MeshletTriangles.Resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));
    }

    for (const meshopt_Meshlet& meshlet : m_Meshlets)
    {
        meshopt_optimizeMeshlet(&m_MeshletVertices[meshlet.vertex_offset], &m_MeshletTriangles[meshlet.triangle_offset], meshlet.triangle_count, meshlet.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(&m_MeshletVertices[meshlet.vertex_offset], &m_MeshletTriangles[meshlet.triangle_offset], meshlet.triangle_count,
                                                                   &m_Vertices[0].position.x, m_Vertices.Size(), sizeof(Vertex));
        m_MeshletBounds.Add(bounds);
    }
}

float4x4 Primitive::GetTransform() const
{
    return m_ParentMesh->GetTransform();
}
