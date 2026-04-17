// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

float3 NormalizeSunTint(float3 sunColor)
{
    float3 tint = max(sunColor, 0.0.xxx);
    float luminance = dot(tint, float3(0.2126f, 0.7152f, 0.0722f));
    return (luminance > 1e-4f) ? (tint / luminance) : 0.0.xxx;
}
