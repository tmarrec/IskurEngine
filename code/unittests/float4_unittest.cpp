// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../common/math/float4.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("float4 constructors", "[float4]")
{
    SECTION("Default constructor initializes to (0, 0, 0, 0)")
    {
        float4 v;
        REQUIRE_THAT(v.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.z, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(v.w, WithinAbs(0.0f, 1e-5f));
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        float4 v(1.0f, 2.0f, 3.0f, 4.0f);
        REQUIRE_THAT(v.x, WithinAbs(1.0f, 1e-5f));
        REQUIRE_THAT(v.y, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(v.z, WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(v.w, WithinAbs(4.0f, 1e-5f));
    }
}

TEST_CASE("float4 addition", "[float4]")
{
    float4 a(1.0f, 2.0f, 3.0f, 4.0f);
    float4 b(5.0f, 6.0f, 7.0f, 8.0f);

    SECTION("Operator+ works correctly")
    {
        float4 c = a + b;
        REQUIRE_THAT(c.x, WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(8.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(10.0f, 1e-5f));
        REQUIRE_THAT(c.w, WithinAbs(12.0f, 1e-5f));
    }

    SECTION("Operator+= works correctly")
    {
        a += b;
        REQUIRE_THAT(a.x, WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(8.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(10.0f, 1e-5f));
        REQUIRE_THAT(a.w, WithinAbs(12.0f, 1e-5f));
    }
}

TEST_CASE("float4 subtraction", "[float4]")
{
    float4 a(5.0f, 6.0f, 7.0f, 8.0f);
    float4 b(1.0f, 2.0f, 3.0f, 4.0f);

    SECTION("Operator- works correctly")
    {
        float4 c = a - b;
        REQUIRE_THAT(c.x, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.w, WithinAbs(4.0f, 1e-5f));
    }

    SECTION("Operator-= works correctly")
    {
        a -= b;
        REQUIRE_THAT(a.x, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.w, WithinAbs(4.0f, 1e-5f));
    }
}

TEST_CASE("float4 scalar multiplication", "[float4]")
{
    float4 a(1.0f, 2.0f, 3.0f, 4.0f);
    f32 scalar = 2.0f;

    SECTION("Operator* works correctly")
    {
        float4 c = a * scalar;
        REQUIRE_THAT(c.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(c.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(c.z, WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(c.w, WithinAbs(8.0f, 1e-5f));
    }

    SECTION("Operator*= works correctly")
    {
        a *= scalar;
        REQUIRE_THAT(a.x, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(a.y, WithinAbs(4.0f, 1e-5f));
        REQUIRE_THAT(a.z, WithinAbs(6.0f, 1e-5f));
        REQUIRE_THAT(a.w, WithinAbs(8.0f, 1e-5f));
    }
}

TEST_CASE("float4 normalization", "[float4]")
{
    float4 a(1.0f, 2.0f, 3.0f, 4.0f);

    SECTION("Normalized works correctly")
    {
        float4 normalized = a.Normalized();
        REQUIRE_THAT(normalized.x, WithinRel(0.1825741f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinRel(0.3651483f, 1e-5f));
        REQUIRE_THAT(normalized.z, WithinRel(0.5477225f, 1e-5f));
        REQUIRE_THAT(normalized.w, WithinRel(0.7302967f, 1e-5f));
    }

    SECTION("Normalization of zero vector returns (0, 0, 0, 0)")
    {
        float4 zero;
        float4 normalized = zero.Normalized();
        REQUIRE_THAT(normalized.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.z, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(normalized.w, WithinAbs(0.0f, 1e-5f));
    }
}

TEST_CASE("float4 dot product", "[float4]")
{
    float4 a(1.0f, 2.0f, 3.0f, 4.0f);
    float4 b(5.0f, 6.0f, 7.0f, 8.0f);

    SECTION("Dot works correctly")
    {
        REQUIRE_THAT(float4::Dot(a, b), WithinAbs(70.0f, 1e-5f));
    }
}

TEST_CASE("float4 unary negation", "[float4]")
{
    SECTION("Unary negation works correctly")
    {
        float4 a(1.0f, -2.0f, 3.0f, -4.0f);
        float4 negated = -a;
        REQUIRE_THAT(negated.x, WithinAbs(-1.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(2.0f, 1e-5f));
        REQUIRE_THAT(negated.z, WithinAbs(-3.0f, 1e-5f));
        REQUIRE_THAT(negated.w, WithinAbs(4.0f, 1e-5f));
    }

    SECTION("Unary negation of zero vector returns zero")
    {
        float4 zero;
        float4 negated = -zero;
        REQUIRE_THAT(negated.x, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.y, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.z, WithinAbs(0.0f, 1e-5f));
        REQUIRE_THAT(negated.w, WithinAbs(0.0f, 1e-5f));
    }
}
