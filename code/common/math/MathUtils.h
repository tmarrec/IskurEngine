// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

constexpr f32 IE_E = 2.71828182845904523536f;        // e
constexpr f32 IE_LOG2E = 1.44269504088896340736f;    // log2(e)
constexpr f32 IE_LOG10E = 0.434294481903251827651f;  // log10(e)
constexpr f32 IE_LN2 = 0.693147180559945309417f;     // ln(2)
constexpr f32 IE_LN10 = 2.30258509299404568402f;     // ln(10)
constexpr f32 IE_PI = 3.14159265358979323846f;       // pi
constexpr f32 IE_PI_2 = 1.57079632679489661923f;     // pi/2
constexpr f32 IE_PI_4 = 0.785398163397448309616f;    // pi/4
constexpr f32 IE_1_PI = 0.318309886183790671538f;    // 1/pi
constexpr f32 IE_2_PI = 0.636619772367581343076f;    // 2/pi
constexpr f32 IE_2_SQRTPI = 1.12837916709551257390f; // 2/sqrt(pi)
constexpr f32 IE_SQRT2 = 1.41421356237309504880f;    // sqrt(2)
constexpr f32 IE_SQRT1_2 = 0.707106781186547524401f; // 1/sqrt(2)

template <typename T> constexpr T IE_Clamp(T v, T lo, T hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

template <typename T> constexpr T IE_ToRadians(T degrees)
{
    return degrees * static_cast<T>(IE_PI) / static_cast<T>(180);
}

template <typename T> constexpr T IE_ToDegrees(T radians)
{
    return radians * static_cast<T>(180) / static_cast<T>(IE_PI);
}

constexpr u32 IE_DivRoundUp(u32 value, u32 divisor)
{
    return (value + divisor - 1) / divisor;
}

constexpr u32 IE_AlignUp(u32 value, u32 alignment)
{
    return (value + alignment - 1u) / alignment * alignment;
}