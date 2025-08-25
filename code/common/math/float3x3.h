// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "float3.h"

class float3x3
{
  public:
    float3x3()
    {
        for (float3& row : m_Rows)
        {
            row = float3();
        }
    }

    float3x3(const float3& row0, const float3& row1, const float3& row2)
    {
        m_Rows[0] = row0;
        m_Rows[1] = row1;
        m_Rows[2] = row2;
    }

    // Matrix * Matrix
    float3x3 operator*(const float3x3& other) const
    {
        float3x3 result;
        const float3x3 otherT = other.Transposed();
        for (int i = 0; i < 3; ++i)
        {
            result[i][0] = float3::Dot(m_Rows[i], otherT[0]);
            result[i][1] = float3::Dot(m_Rows[i], otherT[1]);
            result[i][2] = float3::Dot(m_Rows[i], otherT[2]);
        }
        return result;
    }

    float3x3 Transposed() const
    {
        return {float3(m_Rows[0][0], m_Rows[1][0], m_Rows[2][0]), float3(m_Rows[0][1], m_Rows[1][1], m_Rows[2][1]), float3(m_Rows[0][2], m_Rows[1][2], m_Rows[2][2])};
    }

    static float3x3 Identity()
    {
        return {float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f)};
    }

    float3x3 Inversed() const
    {
        // Row-major elements
        const float a = m_Rows[0][0], b = m_Rows[0][1], c = m_Rows[0][2];
        const float d = m_Rows[1][0], e = m_Rows[1][1], f = m_Rows[1][2];
        const float g = m_Rows[2][0], h = m_Rows[2][1], i = m_Rows[2][2];

        // Cofactors (NOT adjugate yet)
        const float C00 = (e * i - f * h);
        const float C01 = -(d * i - f * g);
        const float C02 = (d * h - e * g);

        const float C10 = -(b * i - c * h);
        const float C11 = (a * i - c * g);
        const float C12 = -(a * h - b * g);

        const float C20 = (b * f - c * e);
        const float C21 = -(a * f - c * d);
        const float C22 = (a * e - b * d);

        // Determinant (row 0 expansion)
        const float det = a * C00 + b * C01 + c * C02;
        if (fabsf(det) < 1e-8f)
        {
            // Singular: mirror float4x4 behavior
            return Identity();
        }
        const float invDet = 1.0f / det;

        // adj(A) = cofactor(A)^T  -> place cofactors as columns in the result rows
        float3x3 inv;
        inv[0] = float3(C00, C10, C20) * invDet;
        inv[1] = float3(C01, C11, C21) * invDet;
        inv[2] = float3(C02, C12, C22) * invDet;
        return inv;
    }

    float3& operator[](int row)
    {
        return m_Rows[row];
    }
    const float3& operator[](int row) const
    {
        return m_Rows[row];
    }

  private:
    float3 m_Rows[3];
};
