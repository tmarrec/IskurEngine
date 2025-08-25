// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"
#include "ffx_api/ffx_upscale.h"

namespace IE_Constants
{
constexpr u32 frameInFlightCount = 3;
constexpr FfxApiUpscaleQualityMode upscalingMode = FFX_UPSCALE_QUALITY_MODE_NATIVEAA;

} // namespace IE_Constants