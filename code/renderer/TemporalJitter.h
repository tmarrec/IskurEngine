// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

namespace TemporalJitter
{
struct Sample
{
    f32 jitterNormX = 0.0f;
    f32 jitterNormY = 0.0f;
};

Sample ComputeHaltonJitter(u32 frameIndex, u32 renderWidth, u32 renderHeight, u32 phaseCount = 8);
} // namespace TemporalJitter
