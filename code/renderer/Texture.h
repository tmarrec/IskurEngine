// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "GpuResource.h"

struct Texture : public GpuResource
{
    u32 srvIndex = UINT32_MAX;
    u32 uavIndex = UINT32_MAX;
};

struct RenderTarget : public GpuResource
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    u32 srvIndex = UINT32_MAX;
};

struct DepthTexture : public GpuResource
{
    ComPtr<D3D12MA::Allocation> allocation;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    u32 srvIndex = UINT32_MAX;
};
