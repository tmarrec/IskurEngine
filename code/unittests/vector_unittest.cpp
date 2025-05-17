// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../common/Vector.h"

TEST_CASE("Vector<int> basic operations", "[Vector]")
{
    SECTION("Default constructor")
    {
        Vector<int> v;
        REQUIRE(v.Empty());
        REQUIRE(v.Size() == 0u);
        REQUIRE(v.Capacity() == 0u);
        REQUIRE(v.Data() == nullptr);
    }

    SECTION("Size constructor")
    {
        Vector<int> v(5);
        REQUIRE(!v.Empty());
        REQUIRE(v.Size() == 5u);
        REQUIRE(v.Capacity() == 5u);
        // Default-initialized ints are indeterminate, but Data() should be valid
        REQUIRE(v.Data() != nullptr);
    }

    SECTION("Initializer-list constructor and indexing")
    {
        Vector<int> v{1, 2, 3, 4};
        REQUIRE(v.Size() == 4u);
        REQUIRE(v.Capacity() == 4u);
        REQUIRE(v[0] == 1);
        REQUIRE(v[1] == 2);
        REQUIRE(v.Back() == 4);
    }

    SECTION("Copy constructor")
    {
        Vector<int> original{10, 20, 30};
        Vector<int> copy(original);
        REQUIRE(copy.Size() == original.Size());
        for (u32 i = 0; i < copy.Size(); ++i)
        {
            REQUIRE(copy[i] == original[i]);
        }
    }

    SECTION("Move constructor")
    {
        Vector<int> src{5, 6, 7};
        Vector<int> dst(std::move(src));
        REQUIRE(dst.Size() == 3u);
        REQUIRE(dst[0] == 5);
        REQUIRE(src.Empty());
        REQUIRE(src.Data() == nullptr);
    }

    SECTION("Copy assignment")
    {
        Vector<int> a{1, 2};
        Vector<int> b{3, 4, 5};
        a = b;
        REQUIRE(a.Size() == b.Size());
        for (u32 i = 0; i < a.Size(); ++i)
        {
            REQUIRE(a[i] == b[i]);
        }
    }

    SECTION("Move assignment")
    {
        Vector<int> a{9, 8};
        Vector<int> b{7, 6, 5};
        a = std::move(b);
        REQUIRE(a.Size() == 3u);
        REQUIRE(a.Back() == 5);
        REQUIRE(b.Empty());
    }

    SECTION("Add and dynamic growth")
    {
        Vector<int> v;
        for (u8 i = 0; i < 10; ++i)
        {
            v.Add(i);
            REQUIRE(v.Back() == i);
            REQUIRE(v.Size() == static_cast<u32>(i + 1));
            REQUIRE(v.Capacity() >= v.Size());
        }
    }

    SECTION("Resize and Clear")
    {
        Vector<int> v{1, 2, 3};
        v.Resize(5);
        REQUIRE(v.Size() == 5u);
        REQUIRE(v.Capacity() >= 5u);
        v.Clear();
        REQUIRE(v.Empty());
        REQUIRE(v.Size() == 0u);
    }

    SECTION("Find existing and non-existing values")
    {
        Vector<int> v{10, 20, 30, 40};
        int* found = v.Find(30);
        REQUIRE(found != nullptr);
        REQUIRE(*found == 30);
        const Vector<int> cv = v;
        const int* cfound = cv.Find(99);
        REQUIRE(cfound == nullptr);
    }

    SECTION("ByteSize reports correct total")
    {
        Vector<int> v{1, 2, 3, 4};
        REQUIRE(v.ByteSize() == v.Size() * sizeof(int));
    }

    SECTION("Begin/End iteration")
    {
        Vector<int> v{3, 1, 4, 1, 5};
        u32 sum = 0;
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            sum += *it;
        }
        REQUIRE(sum == 14);
    }
}

TEST_CASE("Vector<int> performance benchmarks", "[Vector][benchmark]")
{
    BENCHMARK("Default construction")
    {
        return Vector<int>();
    };

    BENCHMARK("Push back 1000 ints")
    {
        Vector<int> v;
        for (u16 i = 0; i < 1000; ++i)
        {
            v.Add(i);
        }
        return v;
    };

    BENCHMARK("Copy large vector")
    {
        Vector<int> big;
        big.Resize(2000);
        for (u32 i = 0; i < big.Size(); ++i)
        {
            big[i] = static_cast<int>(i);
        }
        return Vector<int>(big);
    };

    BENCHMARK("Move large vector")
    {
        Vector<int> big;
        big.Resize(2000);
        for (u32 i = 0; i < big.Size(); ++i)
        {
            big[i] = static_cast<int>(i);
        }
        return Vector<int>(std::move(big));
    };
}