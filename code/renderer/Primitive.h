// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <meshoptimizer.h>
#include <tiny_gltf.h>
#include <wrl/client.h>

#include "../common/Types.h"

class Mesh;
struct Buffer;

struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

class Primitive
{
  public:
    Primitive(const tinygltf::Primitive& gltfPrimitive, const SharedPtr<Mesh>& parentMesh);
    void Process(const tinygltf::Model& model);

    float4x4 GetTransform() const;

    Vector<meshopt_Meshlet> m_Meshlets;
    Vector<u32> m_MeshletVertices;
    Vector<u8> m_MeshletTriangles;
    Vector<meshopt_Bounds> m_MeshletBounds;

    Vector<Vertex> m_Vertices;
    Vector<u32> m_Indices;

    SharedPtr<Buffer> m_VerticesBuffer;
    SharedPtr<Buffer> m_MeshletsBuffer;
    SharedPtr<Buffer> m_MeshletVerticesBuffer;
    SharedPtr<Buffer> m_MeshletTrianglesBuffer;
    SharedPtr<Buffer> m_MeshletBoundsBuffer;

    tinygltf::Primitive m_GltfPrimitive;
    SharedPtr<Mesh> m_ParentMesh;

    // RT
    ComPtr<ID3D12Resource> m_VertexBuffer;
    ComPtr<ID3D12Resource> m_IndexBuffer;
    ComPtr<ID3D12Resource> m_BLAS;
    ComPtr<ID3D12Resource> m_ScratchResource;

    u32 m_MaterialIdx = UINT_MAX;
};