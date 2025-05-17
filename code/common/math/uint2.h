// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class uint2
{
  public:
    uint2() : x(0), y(0)
    {
    }

    uint2(unsigned int x, unsigned int y) : x(x), y(y)
    {
    }

    uint2 operator+(const uint2& other) const
    {
        return {x + other.x, y + other.y};
    }

    uint2& operator+=(const uint2& other)
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    uint2 operator-(const uint2& other) const
    {
        return {x - other.x, y - other.y};
    }

    uint2& operator-=(const uint2& other)
    {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    uint2 operator-() const
    {
        return {static_cast<unsigned int>(-static_cast<int>(x)), static_cast<unsigned int>(-static_cast<int>(y))};
    }

    uint2 operator*(unsigned int scalar) const
    {
        return {x * scalar, y * scalar};
    }

    uint2& operator*=(unsigned int scalar)
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    uint2 operator/(unsigned int scalar) const
    {
        return {x / scalar, y / scalar};
    }

    uint2& operator/=(unsigned int scalar)
    {
        x /= scalar;
        y /= scalar;
        return *this;
    }

    unsigned int x;
    unsigned int y;
};
