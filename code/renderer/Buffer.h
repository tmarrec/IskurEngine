// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "../common/Types.h"

struct Buffer
{
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> buffer;
    u32 srvIndex = 0;
    u32 uavIndex = 0;
    u32 numElements = 0;
};