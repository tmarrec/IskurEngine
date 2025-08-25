// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/catch_test_macros.hpp>

#include "../common/Array.h"

TEST_CASE("Array<i32, 5> basic operations", "[Array]")
{
    SECTION("Static Size")
    {
        REQUIRE(Array<i32, 5>::Size() == 5u);
    }

    SECTION("Default constructor")
    {
        Array<i32, 5> a;
        REQUIRE(a.Data() != nullptr);
        REQUIRE(a.Size() == 5u);
    }

    SECTION("Initializer-list constructor and indexing")
    {
        Array<i32, 4> a{7, 8, 9, 10};
        REQUIRE(a.Size() == 4u);
        REQUIRE(a[0] == 7);
        REQUIRE(a[1] == 8);
        REQUIRE(a[3] == 10);
    }

    SECTION("Front and Back")
    {
        Array<i32, 3> a{1, 2, 3};
        REQUIRE(a.Front() == 1);
        REQUIRE(a.Back() == 3);
    }

    SECTION("Const accessors")
    {
        const Array<i32, 3> a{4, 5, 6};
        REQUIRE(a[0] == 4);
        REQUIRE(a.Front() == 4);
        REQUIRE(a.Back() == 6);
    }

    SECTION("Begin/End iteration")
    {
        Array<i32, 5> a{1, 1, 1, 1, 1};
        i32 sum = 0;
        for (auto it = a.begin(); it != a.end(); ++it)
        {
            sum += *it;
        }
        REQUIRE(sum == 5);
    }
}