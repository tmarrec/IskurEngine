// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Buffer.h"
#include "CPUGPU.h"

struct Primitive
{
    u32 materialIdx = 0;

    // GPU buffers (bindless indices)
    SharedPtr<Buffer> vertices; // stride sizeof(Vertex)
    SharedPtr<Buffer> meshlets; // bytes
    SharedPtr<Buffer> mlVerts;  // u32
    SharedPtr<Buffer> mlTris;   // bytes
    SharedPtr<Buffer> mlBounds; // sizeof(meshopt_Bounds)

    u32 meshletCount = 0;

    // (For ray tracing build)
    const Vertex* cpuVertices = nullptr;
    u32 vertexCount = 0;
    const u32* cpuIndices = nullptr;
    u32 indexCount = 0;

    // BLAS resources
    ComPtr<ID3D12Resource> blas;
    ComPtr<D3D12MA::Allocation> blasAlloc;
    ComPtr<ID3D12Resource> scratch;
    ComPtr<D3D12MA::Allocation> scratchAlloc;

    // temp upload for RT (optional to keep around)
    ComPtr<ID3D12Resource> rtVB;
    ComPtr<D3D12MA::Allocation> rtVBAlloc;
    ComPtr<ID3D12Resource> rtIB;
    ComPtr<D3D12MA::Allocation> rtIBAlloc;
};