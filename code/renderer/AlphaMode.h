// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "../common/Types.h"

enum AlphaMode : u32
{
    AlphaMode_Opaque,
    AlphaMode_Blend,
    AlphaMode_Mask,

    AlphaMode_Count,
};
