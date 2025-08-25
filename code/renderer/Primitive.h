// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <tiny_gltf.h>
#include <wrl/client.h>

#include "../common/Types.h"
#include "CPUGPU.h"
#include "D3D12MemAlloc.h"

class Mesh;
struct Buffer;

class Primitive
{
  public:
    Primitive(const tinygltf::Primitive& gltfPrimitive, const SharedPtr<Mesh>& parentMesh, i32 meshIndex, i32 primIndex);
    void Process();

    float4x4 GetTransform() const;

    Vector<Meshlet> m_Meshlets;
    Vector<u32> m_MeshletVertices;
    Vector<u8> m_MeshletTriangles;
    Vector<MeshletBounds> m_MeshletBounds;

    Vector<Vertex> m_Vertices;
    Vector<u32> m_Indices;

    u32 m_MaterialIdx = UINT_MAX;

    tinygltf::Primitive m_GltfPrimitive;
    WeakPtr<Mesh> m_ParentMesh;

    SharedPtr<Buffer> m_VerticesBuffer;
    SharedPtr<Buffer> m_MeshletsBuffer;
    SharedPtr<Buffer> m_MeshletVerticesBuffer;
    SharedPtr<Buffer> m_MeshletTrianglesBuffer;
    SharedPtr<Buffer> m_MeshletBoundsBuffer;

    // RT
    ComPtr<D3D12MA::Allocation> m_VertexBufferAlloc;
    ComPtr<ID3D12Resource> m_VertexBuffer;

    ComPtr<D3D12MA::Allocation> m_IndexBufferAlloc;
    ComPtr<ID3D12Resource> m_IndexBuffer;

    ComPtr<D3D12MA::Allocation> m_BLASAlloc;
    ComPtr<ID3D12Resource> m_BLAS;

    ComPtr<D3D12MA::Allocation> m_ScratchResourceAlloc;
    ComPtr<ID3D12Resource> m_ScratchResource;

    i32 m_MeshIndex = -1;
    i32 m_PrimIndex = -1;
};