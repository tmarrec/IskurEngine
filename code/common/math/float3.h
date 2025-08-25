// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class float3
{
  public:
    float3() : x(0.0f), y(0.0f), z(0.0f)
    {
    }

    float3(float x, float y, float z) : x(x), y(y), z(z)
    {
    }

    float3 operator+(const float3& other) const
    {
        return {x + other.x, y + other.y, z + other.z};
    }

    float3& operator+=(const float3& other)
    {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    float3 operator-(const float3& other) const
    {
        return {x - other.x, y - other.y, z - other.z};
    }

    float3& operator-=(const float3& other)
    {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    float3 operator-() const
    {
        return {-x, -y, -z};
    }

    float3 operator*(float scalar) const
    {
        return {x * scalar, y * scalar, z * scalar};
    }

    float3& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
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
        return sqrtf(x * x + y * y + z * z);
    }

    float Length2() const
    {
        return x * x + y * y + z * z;
    }

    float3 Normalized() const
    {
        const float length = Length();
        if (length > 0.0f)
        {
            return {x / length, y / length, z / length};
        }
        return {0, 0, 0};
    }

    static float3 Cross(const float3& a, const float3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    static float Dot(const float3& a, const float3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    float x;
    float y;
    float z;
};
