// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class float2
{
  public:
    float2() : x(0.0f), y(0.0f)
    {
    }
    float2(float x, float y) : x(x), y(y)
    {
    }

    float2 operator+(const float2& other) const
    {
        return {x + other.x, y + other.y};
    }

    float2& operator+=(const float2& other)
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    float2 operator-(const float2& other) const
    {
        return {x - other.x, y - other.y};
    }

    float2& operator-=(const float2& other)
    {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    float2 operator-() const
    {
        return {-x, -y};
    }

    float2 operator*(float scalar) const
    {
        return {x * scalar, y * scalar};
    }

    float2& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    float Length() const
    {
        return sqrtf(x * x + y * y);
    }

    float Length2() const
    {
        return x * x + y * y;
    }

    float2 Normalized() const
    {
        const float length = Length();
        if (length > 0.0f)
        {
            return {x / length, y / length};
        }
        return {0, 0};
    }

    static float Dot(const float2& a, const float2& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    float x;
    float y;
};