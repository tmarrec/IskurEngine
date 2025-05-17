// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>

#include "../common/Vector.h"

struct RenderTarget
{
    Vector<ID3D12Resource*> resource;
    ID3D12DescriptorHeap* heap;
};