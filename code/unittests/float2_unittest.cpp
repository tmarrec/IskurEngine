// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../common/math/float2.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("float2 constructors", "[float2]")
{
    SECTION("Default constructor initializes to (0, 0)")
    {
        float2 v;
        REQUIRE_THAT(v.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(0.0f, 1e-5f));
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        float2 v(1.0f, 2.0f);
        REQUIRE_THAT(v.x, WithinAbs(1.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(2.0f, 1e-5f));
    }
}

TEST_CASE("float2 addition", "[float2]")
{
    float2 a(1.0f, 2.0f);
    float2 b(3.0f, 4.0f);

    SECTION("Operator+ works correctly")
    {
        float2 c = a + b;
        REQUIRE_THAT(c.x, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(6.0f, 1e-5f));
    }

    SECTION("Operator+= works correctly")
    {
        a += b;
        REQUIRE_THAT(a.x, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(6.0f, 1e-5f));
    }
}

TEST_CASE("float2 subtraction", "[float2]")
{
    float2 a(4.0f, 6.0f);
    float2 b(1.0f, 2.0f);

    SECTION("Operator- works correctly")
    {
        float2 c = a - b;
        REQUIRE_THAT(c.x, WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
    }

    SECTION("Operator-= works correctly")
    {
        a -= b;
        REQUIRE_THAT(a.x, WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
    }
}

TEST_CASE("float2 scalar multiplication", "[float2]")
{
    float2 a(1.0f, 2.0f);
    f32 scalar = 2.0f;

    SECTION("Operator* works correctly")
    {
        float2 c = a * scalar;
        REQUIRE_THAT(c.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
    }

    SECTION("Operator*= works correctly")
    {
        a *= scalar;
        REQUIRE_THAT(a.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
    }
}

TEST_CASE("float2 normalization", "[float2]")
{
    float2 a(3.0f, 4.0f);

    SECTION("Normalized works correctly")
    {
        float2 normalized = a.Normalized();
        REQUIRE_THAT(normalized.x, WithinRel(0.6f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinRel(0.8f, 1e-5f));
    }

    SECTION("Normalization of zero vector returns (0, 0)")
    {
        float2 zero;
        float2 normalized = zero.Normalized();
        REQUIRE_THAT(normalized.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinAbs(0.0f, 1e-5f));
    }
}

TEST_CASE("float2 dot product", "[float2]")
{
    float2 a(1.0f, 2.0f);
    float2 b(3.0f, 4.0f);

    SECTION("Dot works correctly")
    {
        REQUIRE_THAT(float2::Dot(a, b), WithinAbs(11.0f, 1e-5f));
    }
}

TEST_CASE("float2 unary negation", "[float2]")
{
    SECTION("Unary negation works correctly")
    {
        float2 a(1.0f, -2.0f);
        float2 negated = -a;
        REQUIRE_THAT(negated.x, WithinAbs(-1.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(2.0f, 1e-5f));
    }

    SECTION("Unary negation of zero vector returns zero")
    {
        float2 zero;
        float2 negated = -zero;
        REQUIRE_THAT(negated.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(0.0f, 1e-5f));
    }
}

TEST_CASE("float2 length functions", "[float2]")
{
    SECTION("Length and squared length are computed correctly")
    {
        float2 v(3.0f, 4.0f);
        REQUIRE_THAT(v.Length(), WithinAbs(5.0f, 1e-5f));
        REQUIRE_THAT(v.Length2(), WithinAbs(25.0f, 1e-5f));
    }
}
