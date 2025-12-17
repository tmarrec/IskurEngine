// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Buffer.h"
#include "shaders/CPUGPU.h"

struct Primitive
{
    u32 materialIdx = 0;

    // GPU buffers
    SharedPtr<Buffer> vertices; // stride sizeof(Vertex)
    SharedPtr<Buffer> meshlets; // bytes
    SharedPtr<Buffer> mlVerts;  // u32
    SharedPtr<Buffer> mlTris;   // bytes
    SharedPtr<Buffer> mlBounds; // sizeof(meshopt_Bounds)

    u32 meshletCount = 0;

    const Vertex* cpuVertices = nullptr;
    u32 vertexCount = 0;
    const u32* cpuIndices = nullptr;
    u32 indexCount = 0;

    // BLAS resources
    SharedPtr<Buffer> blas;
    SharedPtr<Buffer> blasScratch;

    SharedPtr<Buffer> rtVertices;
    SharedPtr<Buffer> rtIndices;
};