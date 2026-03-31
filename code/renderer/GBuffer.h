// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Texture.h"

struct GBuffer
{
    static constexpr u32 targetCount = 7;

    RenderTarget albedo;
    RenderTarget normal;
    RenderTarget normalGeo;
    RenderTarget material;
    RenderTarget motionVector;
    RenderTarget ao;
    RenderTarget emissive;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
};

