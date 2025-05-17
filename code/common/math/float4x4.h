// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "float3.h"
#include "float4.h"

class float4x4
{
  public:
    float4x4()
    {
        for (float4& row : m_Rows)
        {
            row = float4();
        }
    }

    float4x4(const float4& row0, const float4& row1, const float4& row2, const float4& row3)
    {
        m_Rows[0] = row0;
        m_Rows[1] = row1;
        m_Rows[2] = row2;
        m_Rows[3] = row3;
    }

    float4x4 operator*(const float4x4& other) const
    {
        float4x4 result;
        const float4x4 otherT = other.Transposed();
        for (int i = 0; i < 4; ++i)
        {
            result[i][0] = float4::Dot(m_Rows[i], otherT[0]);
            result[i][1] = float4::Dot(m_Rows[i], otherT[1]);
            result[i][2] = float4::Dot(m_Rows[i], otherT[2]);
            result[i][3] = float4::Dot(m_Rows[i], otherT[3]);
        }
        return result;
    }

    float4x4 Transposed() const
    {
        return {float4(m_Rows[0][0], m_Rows[1][0], m_Rows[2][0], m_Rows[3][0]), float4(m_Rows[0][1], m_Rows[1][1], m_Rows[2][1], m_Rows[3][1]),
                float4(m_Rows[0][2], m_Rows[1][2], m_Rows[2][2], m_Rows[3][2]), float4(m_Rows[0][3], m_Rows[1][3], m_Rows[2][3], m_Rows[3][3])};
    }

    static float4x4 LookAtRH(const float3& eyePos, const float3& targetPos, const float3& up)
    {
        const float3 forward = (eyePos - targetPos).Normalized();
        const float3 right = float3::Cross(up, forward).Normalized();
        const float3 newUp = float3::Cross(forward, right);

        return {float4(right.x, newUp.x, forward.x, 0.0f), float4(right.y, newUp.y, forward.y, 0.0f), float4(right.z, newUp.z, forward.z, 0.0f),
                float4(-float3::Dot(right, eyePos), -float3::Dot(newUp, eyePos), -float3::Dot(forward, eyePos), 1.0f)};
    }

    static float4x4 Identity()
    {
        return {float4(1.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 1.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 1.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    static float4x4 PerspectiveFovRH(float fov, float aspect, float nearPlane, float farPlane)
    {
        const float fRange = farPlane / (nearPlane - farPlane);
        const float cotFov = 1.0f / tanf(fov / 2.0f);
        return {float4(cotFov / aspect, 0.0f, 0.0f, 0.0f), float4(0.0f, cotFov, 0.0f, 0.0f), float4(0.0f, 0.0f, fRange, -1.0f), float4(0.0f, 0.0f, fRange * nearPlane, 0.0f)};
    }

    static float4x4 OrthographicRH(float width, float height, float nearPlane, float farPlane)
    {
        const float fRange = 1.0f / (nearPlane - farPlane);
        return {float4(2.0f / width, 0.0f, 0.0f, 0.0f), float4(0.0f, 2.0f / height, 0.0f, 0.0f), float4(0.0f, 0.0f, fRange, 0.0f), float4(0.0f, 0.0f, fRange * nearPlane, 1.0f)};
    }

    float4x4 Inversed() const
    {
        // Flatten our matrix (stored in row-major order)
        float m[16] = {m_Rows[0][0], m_Rows[0][1], m_Rows[0][2], m_Rows[0][3], m_Rows[1][0], m_Rows[1][1], m_Rows[1][2], m_Rows[1][3],
                       m_Rows[2][0], m_Rows[2][1], m_Rows[2][2], m_Rows[2][3], m_Rows[3][0], m_Rows[3][1], m_Rows[3][2], m_Rows[3][3]};

        float inv[16];

        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

        float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

        if (det == 0)
        {
            // Handle singular matrix appropriately. Here we return an identity.
            return Identity();
        }

        det = 1.0f / det;

        float4x4 inverse;
        inverse[0] = float4(inv[0] * det, inv[1] * det, inv[2] * det, inv[3] * det);
        inverse[1] = float4(inv[4] * det, inv[5] * det, inv[6] * det, inv[7] * det);
        inverse[2] = float4(inv[8] * det, inv[9] * det, inv[10] * det, inv[11] * det);
        inverse[3] = float4(inv[12] * det, inv[13] * det, inv[14] * det, inv[15] * det);

        return inverse;
    }

    float4& operator[](int row)
    {
        return m_Rows[row];
    }

    const float4& operator[](int row) const
    {
        return m_Rows[row];
    }

  private:
    float4 m_Rows[4];
};
