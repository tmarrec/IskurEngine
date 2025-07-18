﻿// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

enum class RayTracingResolution : u32
{
    Full = 0,        // Full resolution (x, y)
    FullX_HalfY = 1, // Full resolution in x, half resolution in y
    Half = 2,        // Both x and y at half resolution
    Quarter = 3      // Both x and y at quarter resolution
};

namespace IE_Constants
{
constexpr u32 frameInFlightCount = 3;

constexpr bool enableRaytracedShadows = true;
constexpr RayTracingResolution raytracedShadowsType = RayTracingResolution::FullX_HalfY;
} // namespace IE_Constants