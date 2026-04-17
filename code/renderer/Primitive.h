// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Buffer.h"
#include "shaders/CPUGPU.h"

struct Primitive
{
    // GPU buffers
    SharedPtr<Buffer> vertices; // stride sizeof(Vertex)
    SharedPtr<Buffer> meshlets; // bytes
    SharedPtr<Buffer> mlVerts;  // u32
    SharedPtr<Buffer> mlTris;   // bytes
    SharedPtr<Buffer> mlBounds; // sizeof(MeshletBounds)

    u32 meshletCount = 0;
    XMFLOAT3 localBoundsCenter = {0.0f, 0.0f, 0.0f};
    f32 localBoundsRadius = 0.0f;

    // BLAS resources
    SharedPtr<Buffer> blas;
    SharedPtr<Buffer> blasScratch;
    SharedPtr<Buffer> ommArray;
    SharedPtr<Buffer> ommArrayScratch;
    SharedPtr<Buffer> ommIndices;
    SharedPtr<Buffer> ommDescs;
    SharedPtr<Buffer> ommData;

    SharedPtr<Buffer> rtVertices;
    SharedPtr<Buffer> rtIndices;
};

