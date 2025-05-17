// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class float4
{
  public:
    float4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f)
    {
    }
    float4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w)
    {
    }

    float4 operator+(const float4& other) const
    {
        return {x + other.x, y + other.y, z + other.z, w + other.w};
    }

    float4& operator+=(const float4& other)
    {
        x += other.x;
        y += other.y;
        z += other.z;
        w += other.w;
        return *this;
    }

    float4 operator-(const float4& other) const
    {
        return {x - other.x, y - other.y, z - other.z, w - other.w};
    }

    float4& operator-=(const float4& other)
    {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        w -= other.w;
        return *this;
    }

    float4 operator-() const
    {
        return {-x, -y, -z, -w};
    }

    float4 operator*(float scalar) const
    {
        return {x * scalar, y * scalar, z * scalar, w * scalar};
    }

    float4& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        w *= scalar;
        return *this;
    }

    float& operator[](int index)
    {
        return *(&x + index);
    }

    const float& operator[](int index) const
    {
        return *(&x + index);
    }

    float Length() const
    {
        return sqrtf(x * x + y * y + z * z + w * w);
    }

    float Length2() const
    {
        return x * x + y * y + z * z + w * w;
    }

    float4 Normalized() const
    {
        const float length = Length();
        if (length > 0.0f)
        {
            return {x / length, y / length, z / length, w / length};
        }
        return {0, 0, 0, 0};
    }

    static float Dot(const float4& a, const float4& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    float x;
    float y;
    float z;
    float w;
};