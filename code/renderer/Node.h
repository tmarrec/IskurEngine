// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Mesh.h"

struct Node
{
    float4x4 localTransform;
    SharedPtr<Mesh> mesh;
    Vector<SharedPtr<Node>> nodes;
    SharedPtr<Node> parentNode;
};