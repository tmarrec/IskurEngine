// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "../common/math/uint2.h"

TEST_CASE("uint2 constructors", "[uint2]")
{
    SECTION("Default constructor initializes to (0, 0)")
    {
        uint2 v;
        REQUIRE(v.x == 0u);
        REQUIRE(v.y == 0u);
    }

    SECTION("Parameterized constructor initializes correctly")
    {
        uint2 v(3, 4);
        REQUIRE(v.x == 3u);
        REQUIRE(v.y == 4u);
    }
}

TEST_CASE("uint2 addition", "[uint2]")
{
    uint2 a(1, 2);
    uint2 b(3, 4);

    SECTION("Operator+ works correctly")
    {
        uint2 c = a + b;
        REQUIRE(c.x == 4u);
        REQUIRE(c.y == 6u);
    }

    SECTION("Operator+= works correctly")
    {
        a += b;
        REQUIRE(a.x == 4u);
        REQUIRE(a.y == 6u);
    }

    BENCHMARK("uint2 Addition operator")
    {
        return a + b;
    };
}

TEST_CASE("uint2 subtraction", "[uint2]")
{
    uint2 a(5, 7);
    uint2 b(3, 4);

    SECTION("Operator- works correctly")
    {
        uint2 c = a - b;
        REQUIRE(c.x == 2u);
        REQUIRE(c.y == 3u);
    }

    SECTION("Operator-= works correctly")
    {
        a -= b;
        REQUIRE(a.x == 2u);
        REQUIRE(a.y == 3u);
    }

    BENCHMARK("uint2 Subtraction operator")
    {
        return a - b;
    };
}

TEST_CASE("uint2 unary negation", "[uint2]")
{
    SECTION("Unary negation works correctly")
    {
        uint2 a(1, 2);
        uint2 neg = -a;
        REQUIRE(neg.x == static_cast<unsigned int>(-static_cast<int>(1)));
        REQUIRE(neg.y == static_cast<unsigned int>(-static_cast<int>(2)));
    }

    BENCHMARK("uint2 Unary negation")
    {
        uint2 a(1, 2);
        return -a;
    };
}

TEST_CASE("uint2 scalar multiplication", "[uint2]")
{
    uint2 a(2, 3);
    u8 scalar = 5;

    SECTION("Operator* works correctly")
    {
        uint2 c = a * scalar;
        REQUIRE(c.x == 10u);
        REQUIRE(c.y == 15u);
    }

    SECTION("Operator*= works correctly")
    {
        a *= scalar;
        REQUIRE(a.x == 10u);
        REQUIRE(a.y == 15u);
    }

    BENCHMARK("uint2 Scalar multiplication operator")
    {
        return a * scalar;
    };
}

TEST_CASE("uint2 scalar division", "[uint2]")
{
    uint2 a(10, 20);
    u8 scalar = 2;

    SECTION("Operator/ works correctly")
    {
        uint2 c = a / scalar;
        REQUIRE(c.x == 5u);
        REQUIRE(c.y == 10u);
        // Ensure original remains unchanged
        REQUIRE(a.x == 10u);
        REQUIRE(a.y == 20u);
    }

    SECTION("Operator/= works correctly")
    {
        a /= scalar;
        REQUIRE(a.x == 5u);
        REQUIRE(a.y == 10u);
    }

    BENCHMARK("uint2 Scalar division operator")
    {
        return a / scalar;
    };
}