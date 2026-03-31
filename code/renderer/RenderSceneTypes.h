// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"
#include "shaders/CPUGPU.h"

enum AlphaMode : u8
{
    AlphaMode_Opaque,
    AlphaMode_Blend,
    AlphaMode_Mask,

    AlphaMode_Count,
};

enum CullMode : u8
{
    CullMode_Back,
    CullMode_None,

    CullMode_Count
};

struct PrimitiveRenderData
{
    u32 primIndex;
    PrimitiveConstants primConstants;
};

struct InstanceData
{
    u32 primIndex;
    u32 materialIndex;
    XMFLOAT4X4 world;
};
