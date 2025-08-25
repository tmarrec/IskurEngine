// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../common/math/float3.h"
#include "../common/math/float3x3.h"

using Catch::Matchers::WithinAbs;

static constexpr float EPS = 1e-5f;

TEST_CASE("float3x3 constructors & identity", "[float3x3]")
{
    SECTION("Default constructor initializes to null matrix")
    {
        float3x3 m;
        for (int r = 0; r < 3; ++r)
        {
            REQUIRE_THAT(m[r][0], WithinAbs(0.0f, EPS));
            REQUIRE_THAT(m[r][1], WithinAbs(0.0f, EPS));
            REQUIRE_THAT(m[r][2], WithinAbs(0.0f, EPS));
        }
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        float3 r0(1, 2, 3);
        float3 r1(4, 5, 6);
        float3 r2(7, 8, 9);

        float3x3 m(r0, r1, r2);

        REQUIRE_THAT(m[0][0], WithinAbs(1.0f, EPS));
        REQUIRE_THAT(m[0][1], WithinAbs(2.0f, EPS));
        REQUIRE_THAT(m[0][2], WithinAbs(3.0f, EPS));

        REQUIRE_THAT(m[1][0], WithinAbs(4.0f, EPS));
        REQUIRE_THAT(m[1][1], WithinAbs(5.0f, EPS));
        REQUIRE_THAT(m[1][2], WithinAbs(6.0f, EPS));

        REQUIRE_THAT(m[2][0], WithinAbs(7.0f, EPS));
        REQUIRE_THAT(m[2][1], WithinAbs(8.0f, EPS));
        REQUIRE_THAT(m[2][2], WithinAbs(9.0f, EPS));
    }

    SECTION("Identity is correct")
    {
        float3x3 I = float3x3::Identity();

        REQUIRE_THAT(I[0][0], WithinAbs(1.0f, EPS));
        REQUIRE_THAT(I[0][1], WithinAbs(0.0f, EPS));
        REQUIRE_THAT(I[0][2], WithinAbs(0.0f, EPS));

        REQUIRE_THAT(I[1][0], WithinAbs(0.0f, EPS));
        REQUIRE_THAT(I[1][1], WithinAbs(1.0f, EPS));
        REQUIRE_THAT(I[1][2], WithinAbs(0.0f, EPS));

        REQUIRE_THAT(I[2][0], WithinAbs(0.0f, EPS));
        REQUIRE_THAT(I[2][1], WithinAbs(0.0f, EPS));
        REQUIRE_THAT(I[2][2], WithinAbs(1.0f, EPS));
    }
}

TEST_CASE("float3x3 multiplication", "[float3x3]")
{
    // Use a well-conditioned, invertible matrix
    // A = [ 1 2 3 ; 0 1 4 ; 5 6 0 ]
    float3x3 A(float3(1, 2, 3), float3(0, 1, 4), float3(5, 6, 0));

    // B = [ -2 1 0 ; 3 0 0 ; 4 5 1 ]
    float3x3 B(float3(-2, 1, 0), float3(3, 0, 0), float3(4, 5, 1));

    SECTION("Operator* works correctly")
    {
        float3x3 C = A * B;

        // Expected C = A*B =
        // [ 16 16 3 ;
        //   19 20 4 ;
        //    8  5 0 ]
        REQUIRE_THAT(C[0][0], WithinAbs(16.0f, EPS));
        REQUIRE_THAT(C[0][1], WithinAbs(16.0f, EPS));
        REQUIRE_THAT(C[0][2], WithinAbs(3.0f, EPS));

        REQUIRE_THAT(C[1][0], WithinAbs(19.0f, EPS));
        REQUIRE_THAT(C[1][1], WithinAbs(20.0f, EPS));
        REQUIRE_THAT(C[1][2], WithinAbs(4.0f, EPS));

        REQUIRE_THAT(C[2][0], WithinAbs(8.0f, EPS));
        REQUIRE_THAT(C[2][1], WithinAbs(5.0f, EPS));
        REQUIRE_THAT(C[2][2], WithinAbs(0.0f, EPS));
    }

    SECTION("Multiplying by identity leaves the matrix unchanged")
    {
        float3x3 I = float3x3::Identity();
        float3x3 L = I * A;
        float3x3 R = A * I;

        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                REQUIRE_THAT(L[r][c], WithinAbs(A[r][c], EPS));
                REQUIRE_THAT(R[r][c], WithinAbs(A[r][c], EPS));
            }
        }
    }
}

TEST_CASE("float3x3 transposition", "[float3x3]")
{
    float3x3 M(float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9));

    SECTION("Transposed matrix is correct")
    {
        float3x3 T = M.Transposed();

        REQUIRE_THAT(T[0][0], WithinAbs(1.0f, EPS));
        REQUIRE_THAT(T[0][1], WithinAbs(4.0f, EPS));
        REQUIRE_THAT(T[0][2], WithinAbs(7.0f, EPS));

        REQUIRE_THAT(T[1][0], WithinAbs(2.0f, EPS));
        REQUIRE_THAT(T[1][1], WithinAbs(5.0f, EPS));
        REQUIRE_THAT(T[1][2], WithinAbs(8.0f, EPS));

        REQUIRE_THAT(T[2][0], WithinAbs(3.0f, EPS));
        REQUIRE_THAT(T[2][1], WithinAbs(6.0f, EPS));
        REQUIRE_THAT(T[2][2], WithinAbs(9.0f, EPS));
    }

    SECTION("Double transpose returns original")
    {
        float3x3 TT = M.Transposed().Transposed();
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                REQUIRE_THAT(TT[r][c], WithinAbs(M[r][c], EPS));
            }
        }
    }
}

TEST_CASE("float3x3 inverse", "[float3x3]")
{
    // Same invertible matrix as above
    float3x3 A(float3(1, 2, 3), float3(0, 1, 4), float3(5, 6, 0));

    SECTION("Inversed() returns the mathematical inverse")
    {
        float3x3 inv = A.Inversed();

        // Expected exact integer inverse:
        // A^-1 = [ -24 18  5 ;
        //           20 -15 -4 ;
        //           -5   4  1 ]
        REQUIRE_THAT(inv[0][0], WithinAbs(-24.0f, EPS));
        REQUIRE_THAT(inv[0][1], WithinAbs(18.0f, EPS));
        REQUIRE_THAT(inv[0][2], WithinAbs(5.0f, EPS));

        REQUIRE_THAT(inv[1][0], WithinAbs(20.0f, EPS));
        REQUIRE_THAT(inv[1][1], WithinAbs(-15.0f, EPS));
        REQUIRE_THAT(inv[1][2], WithinAbs(-4.0f, EPS));

        REQUIRE_THAT(inv[2][0], WithinAbs(-5.0f, EPS));
        REQUIRE_THAT(inv[2][1], WithinAbs(4.0f, EPS));
        REQUIRE_THAT(inv[2][2], WithinAbs(1.0f, EPS));
    }

    SECTION("A * A^-1 == I and A^-1 * A == I")
    {
        float3x3 I = float3x3::Identity();
        float3x3 inv = A.Inversed();

        float3x3 L = A * inv;
        float3x3 R = inv * A;

        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                REQUIRE_THAT(L[r][c], WithinAbs(I[r][c], 1e-4f));
                REQUIRE_THAT(R[r][c], WithinAbs(I[r][c], 1e-4f));
            }
        }
    }

    SECTION("Singular matrix inverse falls back to Identity (matches float4x4 behavior)")
    {
        // Two rows equal => singular
        float3x3 S(float3(1, 2, 3), float3(1, 2, 3), float3(0, 0, 0));

        float3x3 invS = S.Inversed();
        float3x3 I = float3x3::Identity();

        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                REQUIRE_THAT(invS[r][c], WithinAbs(I[r][c], EPS));
            }
        }
    }
}
