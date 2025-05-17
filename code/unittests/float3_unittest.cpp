// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../common/math/float3.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("float3 constructors", "[float3]")
{
    SECTION("Default constructor initializes to (0, 0, 0)")
    {
        float3 v;
        REQUIRE_THAT(v.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.z, WithinAbs(0.0f, 1e-5f));
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        float3 v(1.0f, 2.0f, 3.0f);
        REQUIRE_THAT(v.x, WithinAbs(1.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(v.z, WithinAbs(3.0f, 1e-5f));
    }
}

TEST_CASE("float3 addition", "[float3]")
{
    float3 a(1.0f, 2.0f, 3.0f);
    float3 b(4.0f, 5.0f, 6.0f);

    SECTION("Operator+ works correctly")
    {
        float3 c = a + b;
        REQUIRE_THAT(c.x, WithinAbs(5.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(7.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(9.0f, 1e-5f));
    }

    SECTION("Operator+= works correctly")
    {
        a += b;
        REQUIRE_THAT(a.x, WithinAbs(5.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(7.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(9.0f, 1e-5f));
    }

    BENCHMARK("Addition operator")
    {
        return a + b;
    };

    BENCHMARK("Addition-assignment operator")
    {
        float3 temp = a;
        temp += b;
        return temp;
    };
}

TEST_CASE("float3 subtraction", "[float3]")
{
    float3 a(4.0f, 6.0f, 8.0f);
    float3 b(1.0f, 2.0f, 3.0f);

    SECTION("Operator- works correctly")
    {
        float3 c = a - b;
        REQUIRE_THAT(c.x, WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(5.0f, 1e-5f));
    }

    SECTION("Operator-= works correctly")
    {
        a -= b;
        REQUIRE_THAT(a.x, WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(5.0f, 1e-5f));
    }

    BENCHMARK("Subtraction operator")
    {
        return a - b;
    };

    BENCHMARK("Subtraction-assignment operator")
    {
        float3 temp = a;
        temp -= b;
        return temp;
    };
}

TEST_CASE("float3 scalar multiplication", "[float3]")
{
    float3 a(1.0f, 2.0f, 3.0f);
    f32 scalar = 2.0f;

    SECTION("Operator* works correctly")
    {
        float3 c = a * scalar;
        REQUIRE_THAT(c.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(6.0f, 1e-5f));
    }

    SECTION("Operator*= works correctly")
    {
        a *= scalar;
        REQUIRE_THAT(a.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(6.0f, 1e-5f));
    }

    BENCHMARK("Scalar multiplication operator")
    {
        return a * scalar;
    };

    BENCHMARK("Scalar multiplication-assignment operator")
    {
        float3 temp = a;
        temp *= scalar;
        return temp;
    };
}

TEST_CASE("float3 normalization", "[float3]")
{
    float3 a(3.0f, 4.0f, 0.0f);

    SECTION("Normalized works correctly")
    {
        float3 normalized = a.Normalized();
        REQUIRE_THAT(normalized.x, WithinRel(0.6f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinRel(0.8f, 1e-5f));
        REQUIRE_THAT(normalized.z, WithinRel(0.0f, 1e-5f));
    }

    SECTION("Normalization of zero vector returns (0, 0, 0)")
    {
        float3 zero;
        float3 normalized = zero.Normalized();
        REQUIRE_THAT(normalized.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.z, WithinAbs(0.0f, 1e-5f));
    }

    BENCHMARK("Normalization")
    {
        return a.Normalized();
    };

    SECTION("Length and squared length are computed correctly")
    {
        float3 v(3.0f, 4.0f, 12.0f);
        REQUIRE_THAT(v.Length(), WithinAbs(13.0f, 1e-5f));
        REQUIRE_THAT(v.Length2(), WithinAbs(169.0f, 1e-5f));
    }
}

TEST_CASE("float3 dot product", "[float3]")
{
    float3 a(1.0f, 2.0f, 3.0f);
    float3 b(4.0f, 5.0f, 6.0f);

    SECTION("Dot works correctly")
    {
        REQUIRE_THAT(float3::Dot(a, b), WithinAbs(32.0f, 1e-5f));
    }

    BENCHMARK("Dot product")
    {
        return float3::Dot(a, b);
    };
}

TEST_CASE("float3 cross product", "[float3]")
{
    float3 a(1.0f, 0.0f, 0.0f);
    float3 b(0.0f, 1.0f, 0.0f);

    SECTION("Cross works correctly")
    {
        float3 cross = float3::Cross(a, b);
        REQUIRE_THAT(cross.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(cross.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(cross.z, WithinAbs(1.0f, 1e-5f));
    }

    BENCHMARK("Cross product")
    {
        return float3::Cross(a, b);
    };
}

TEST_CASE("float3 unary negation", "[float3]")
{
    SECTION("Unary negation works correctly")
    {
        float3 a(1.0f, -2.0f, 3.0f);
        float3 negated = -a;
        REQUIRE_THAT(negated.x, WithinAbs(-1.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(negated.z, WithinAbs(-3.0f, 1e-5f));
    }

    SECTION("Unary negation of zero vector returns zero")
    {
        float3 zero;
        float3 negated = -zero;
        REQUIRE_THAT(negated.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.z, WithinAbs(0.0f, 1e-5f));
    }
}
