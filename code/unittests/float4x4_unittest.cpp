// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <DirectXMath.h>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../common/math/float4x4.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

static DirectX::XMFLOAT4 Store(const DirectX::XMVECTOR& v)
{
    DirectX::XMFLOAT4 f;
    DirectX::XMStoreFloat4(&f, v);
    return f;
}

TEST_CASE("float4x4 constructors", "[float4x4]")
{
    SECTION("Default constructor initializes to null matrix")
    {
        float4x4 m;
        for (u8 i = 0; i < 4; ++i)
        {
            REQUIRE_THAT(m[i][0], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(m[i][1], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(m[i].z, WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(m[i].w, WithinAbs(0.0f, 1e-5f));
        }
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        float4 row0(1.0f, 2.0f, 3.0f, 4.0f);
        float4 row1(5.0f, 6.0f, 7.0f, 8.0f);
        float4 row2(9.0f, 10.0f, 11.0f, 12.0f);
        float4 row3(13.0f, 14.0f, 15.0f, 16.0f);

        float4x4 m(row0, row1, row2, row3);

        REQUIRE_THAT(m[0][0], WithinAbs(1.0f, 1e-5f));
        REQUIRE_THAT(m[0][1], WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(m[0][2], WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(m[0][3], WithinAbs(4.0f, 1e-5f));

        REQUIRE_THAT(m[1][0], WithinAbs(5.0f, 1e-5f));
        REQUIRE_THAT(m[1][1], WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(m[1][2], WithinAbs(7.0f, 1e-5f));
        REQUIRE_THAT(m[1][3], WithinAbs(8.0f, 1e-5f));

        REQUIRE_THAT(m[2][0], WithinAbs(9.0f, 1e-5f));
        REQUIRE_THAT(m[2][1], WithinAbs(10.0f, 1e-5f));
        REQUIRE_THAT(m[2][2], WithinAbs(11.0f, 1e-5f));
        REQUIRE_THAT(m[2][3], WithinAbs(12.0f, 1e-5f));

        REQUIRE_THAT(m[3][0], WithinAbs(13.0f, 1e-5f));
        REQUIRE_THAT(m[3][1], WithinAbs(14.0f, 1e-5f));
        REQUIRE_THAT(m[3][2], WithinAbs(15.0f, 1e-5f));
        REQUIRE_THAT(m[3][3], WithinAbs(16.0f, 1e-5f));
    }
}

TEST_CASE("float4x4 multiplication", "[float4x4]")
{
    float4x4 a(float4(1.0f, 2.0f, 3.0f, 4.0f), float4(5.0f, 6.0f, 7.0f, 8.0f), float4(9.0f, 10.0f, 11.0f, 12.0f), float4(13.0f, 14.0f, 15.0f, 16.0f));

    float4x4 b(float4(16.0f, 15.0f, 14.0f, 13.0f), float4(12.0f, 11.0f, 10.0f, 9.0f), float4(8.0f, 7.0f, 6.0f, 5.0f), float4(4.0f, 3.0f, 2.0f, 1.0f));

    SECTION("Operator* works correctly")
    {
        float4x4 result = a * b;

        REQUIRE_THAT(result[0][0], WithinAbs(80.0f, 1e-5f));
        REQUIRE_THAT(result[0][1], WithinAbs(70.0f, 1e-5f));
        REQUIRE_THAT(result[0][2], WithinAbs(60.0f, 1e-5f));
        REQUIRE_THAT(result[0][3], WithinAbs(50.0f, 1e-5f));

        REQUIRE_THAT(result[1][0], WithinAbs(240.0f, 1e-5f));
        REQUIRE_THAT(result[1][1], WithinAbs(214.0f, 1e-5f));
        REQUIRE_THAT(result[1][2], WithinAbs(188.0f, 1e-5f));
        REQUIRE_THAT(result[1][3], WithinAbs(162.0f, 1e-5f));

        REQUIRE_THAT(result[2][0], WithinAbs(400.0f, 1e-5f));
        REQUIRE_THAT(result[2][1], WithinAbs(358.0f, 1e-5f));
        REQUIRE_THAT(result[2][2], WithinAbs(316.0f, 1e-5f));
        REQUIRE_THAT(result[2][3], WithinAbs(274.0f, 1e-5f));

        REQUIRE_THAT(result[3][0], WithinAbs(560.0f, 1e-5f));
        REQUIRE_THAT(result[3][1], WithinAbs(502.0f, 1e-5f));
        REQUIRE_THAT(result[3][2], WithinAbs(444.0f, 1e-5f));
        REQUIRE_THAT(result[3][3], WithinAbs(386.0f, 1e-5f));
    }

    BENCHMARK("Matrix multiplication operator")
    {
        return a * b;
    };
}

TEST_CASE("float4x4 transposition", "[float4x4]")
{
    float4x4 m(float4(1.0f, 2.0f, 3.0f, 4.0f), float4(5.0f, 6.0f, 7.0f, 8.0f), float4(9.0f, 10.0f, 11.0f, 12.0f), float4(13.0f, 14.0f, 15.0f, 16.0f));

    SECTION("Transposed matrix is correct")
    {
        float4x4 result = m.Transposed();

        REQUIRE_THAT(result[0][0], WithinAbs(1.0f, 1e-5f));
        REQUIRE_THAT(result[0][1], WithinAbs(5.0f, 1e-5f));
        REQUIRE_THAT(result[0][2], WithinAbs(9.0f, 1e-5f));
        REQUIRE_THAT(result[0][3], WithinAbs(13.0f, 1e-5f));

        REQUIRE_THAT(result[1][0], WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(result[1][1], WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(result[1][2], WithinAbs(10.0f, 1e-5f));
        REQUIRE_THAT(result[1][3], WithinAbs(14.0f, 1e-5f));

        REQUIRE_THAT(result[2][0], WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(result[2][1], WithinAbs(7.0f, 1e-5f));
        REQUIRE_THAT(result[2][2], WithinAbs(11.0f, 1e-5f));
        REQUIRE_THAT(result[2][3], WithinAbs(15.0f, 1e-5f));

        REQUIRE_THAT(result[3][0], WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(result[3][1], WithinAbs(8.0f, 1e-5f));
        REQUIRE_THAT(result[3][2], WithinAbs(12.0f, 1e-5f));
        REQUIRE_THAT(result[3][3], WithinAbs(16.0f, 1e-5f));
    }

    BENCHMARK("Transposition")
    {
        return m.Transposed();
    };
}

TEST_CASE("Matrix perspective fov", "[float4x4]")
{
    using namespace DirectX;
    XMMATRIX reference = XMMatrixPerspectiveFovRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);
    float4x4 value = float4x4::PerspectiveFovRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);

    SECTION("Perspective fov matrix is correct")
    {
        for (u8 i = 0; i < 4; ++i)
        {
            XMFLOAT4 r = Store(reference.r[i]);
            REQUIRE_THAT(value[i][0], WithinAbs(r.x, 1e-5f));
            REQUIRE_THAT(value[i][1], WithinAbs(r.y, 1e-5f));
            REQUIRE_THAT(value[i][2], WithinAbs(r.z, 1e-5f));
            REQUIRE_THAT(value[i][3], WithinAbs(r.w, 1e-5f));
        }
    }

    BENCHMARK("Perspective fov")
    {
        return float4x4::PerspectiveFovRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);
    };
}

TEST_CASE("Matrix perspective", "[float4x4]")
{
    using namespace DirectX;
    XMMATRIX reference = XMMatrixOrthographicRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);
    float4x4 value = float4x4::OrthographicRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);

    SECTION("Orthographic matrix is correct")
    {
        for (u8 i = 0; i < 4; ++i)
        {
            XMFLOAT4 r = Store(reference.r[i]);
            REQUIRE_THAT(value[i][0], WithinAbs(r.x, 1e-5f));
            REQUIRE_THAT(value[i][1], WithinAbs(r.y, 1e-5f));
            REQUIRE_THAT(value[i][2], WithinAbs(r.z, 1e-5f));
            REQUIRE_THAT(value[i][3], WithinAbs(r.w, 1e-5f));
        }
    }

    BENCHMARK("Orthographic")
    {
        return float4x4::OrthographicRH(3.14159265358979323846f / 3.0f, 2560.f / 1440.f, 0.1f, 100000.f);
    };
}

TEST_CASE("Matrix LookAt", "[float4x4]")
{
    using namespace DirectX;
    XMVECTOR pos = XMVectorSet(964.963989f, 456.868988f, 305.289001f, 0.0f);
    XMVECTOR targetPosition = XMVectorSet(0.909608006f, -0.108007997f, -0.401183009f, 0.0f);
    XMVECTOR upDirection = XMVectorSet(0.f, 1.0f, 0.f, 0.0f);

    XMMATRIX reference = XMMatrixLookAtRH(pos, targetPosition, upDirection);
    float4x4 value = float4x4::LookAtRH({964.963989f, 456.868988f, 305.289001f}, {0.909608006f, -0.108007997f, -0.401183009f}, {0, 1, 0});

    SECTION("LookAt matrix is correct")
    {
        for (u8 i = 0; i < 4; ++i)
        {
            XMFLOAT4 r = Store(reference.r[i]);
            REQUIRE_THAT(value[i][0], WithinAbs(r.x, 1e-5f));
            REQUIRE_THAT(value[i][1], WithinAbs(r.y, 1e-5f));
            REQUIRE_THAT(value[i][2], WithinAbs(r.z, 1e-5f));
            REQUIRE_THAT(value[i][3], WithinAbs(r.w, 1e-5f));
        }
    }

    BENCHMARK("LookAt")
    {
        return float4x4::LookAtRH({32.0f, 128.0f, -512.0f}, {0, 0, 0}, {0, 1, 0});
    };
}
