// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "../common/Types.h"
#include "AlphaMode.h"

struct Material
{
    f32 metallicFactor;
    f32 roughnessFactor;
    i32 baseColorTextureIndex;
    i32 baseColorSamplerIndex;

    float4 baseColorFactor;

    AlphaMode alphaMode; // could be packed
    f32 alphaCutoff;
    i32 metallicRoughnessTextureIndex;
    i32 metallicRoughnessSamplerIndex;

    i32 normalTextureIndex;
    i32 normalSamplerIndex;
    f32 normalScale;
    i32 pad;
};