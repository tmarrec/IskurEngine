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
    ComPtr<ID3D12Resource> buffer;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle;
    u32 index = 0;
};