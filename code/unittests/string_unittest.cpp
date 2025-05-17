// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "../common/String.h"

TEST_CASE("String basic operations", "[String]")
{
    SECTION("Default constructor")
    {
        String s;
        REQUIRE(s.Empty());
        REQUIRE(s.Size() == 0u);
        REQUIRE(s.Data() != nullptr);
        REQUIRE(s.Data()[0] == '\0');
    }

    SECTION("C-string constructor and indexing")
    {
        String s("hello");
        REQUIRE(!s.Empty());
        REQUIRE(s.Size() == 5u);
        REQUIRE(std::string(s.Data()) == "hello");
        REQUIRE(s[0] == 'h');
        REQUIRE(s[1] == 'e');
        REQUIRE(s[4] == 'o');
    }

    SECTION("Copy constructor")
    {
        String original("test");
        String copy(original);
        REQUIRE(copy == original);
        REQUIRE(copy.Size() == original.Size());
        REQUIRE(std::string(copy.Data()) == std::string(original.Data()));
    }

    SECTION("Move constructor")
    {
        String src("move");
        String dst(std::move(src));
        REQUIRE(dst.Size() == 4u);
        REQUIRE(std::string(dst.Data()) == "move");
        REQUIRE(src.Empty());
        REQUIRE(src.Data() == nullptr);
    }

    SECTION("Copy assignment")
    {
        String a("one"), b("two");
        a = b;
        REQUIRE(a == b);
        REQUIRE(std::string(a.Data()) == "two");
    }

    SECTION("Move assignment")
    {
        String a("foo"), b("bar");
        a = std::move(b);
        REQUIRE(a.Size() == 3u);
        REQUIRE(std::string(a.Data()) == "bar");
        REQUIRE(b.Empty());
        REQUIRE(b.Data() == nullptr);
    }

    SECTION("Append and += operators")
    {
        String s("hello");
        s.Append(" world");
        REQUIRE(std::string(s.Data()) == "hello world");
        REQUIRE(s.Size() == 11u);

        s = String("foo");
        s += "bar";
        REQUIRE(std::string(s.Data()) == "foobar");
        REQUIRE(s.Size() == 6u);

        String part("baz");
        s += part;
        REQUIRE(std::string(s.Data()) == "foobarbaz");
        REQUIRE(s.Size() == 9u);
    }

    SECTION("Reserve behavior")
    {
        String s("abc");
        s.Reserve(2);
        REQUIRE(std::string(s.Data()) == "abc");
        REQUIRE(s.Size() == 3u);

        s.Reserve(10);
        s.Append("defgh");
        REQUIRE(std::string(s.Data()) == "abcdefgh");
        REQUIRE(s.Size() == 8u);
    }

    SECTION("Operator+ concatenation")
    {
        String a("blabla");
        String b("hehe");
        // test that a+b produces a new String and doesn't modify lhs/rhs
        String c = a + b;
        REQUIRE(c == String("blablahehe"));
        REQUIRE(a == String("blabla"));
        REQUIRE(b == String("hehe"));
    }
}

TEST_CASE("String performance benchmarks", "[String][benchmark]")
{
    BENCHMARK("Default construction")
    {
        return String();
    };

    BENCHMARK("Construction from literal")
    {
        return String("benchmark");
    };

    BENCHMARK("Append single characters 100 times")
    {
        String s;
        for (u8 i = 0; i < 100; ++i)
        {
            s.Append("x");
        }
        return s;
    };

    BENCHMARK("Reserve then append 100 chars")
    {
        String s;
        s.Reserve(1000);
        for (u8 i = 0; i < 100; ++i)
        {
            s.Append("y");
        }
        return s;
    };

    BENCHMARK("Copy large string")
    {
        String big;
        for (u16 i = 0; i < 1000; ++i)
        {
            big.Append("z");
        }
        return String(big);
    };

    BENCHMARK("Move large string")
    {
        String big;
        for (u16 i = 0; i < 1000; ++i)
        {
            big.Append("z");
        }
        return String(std::move(big));
    };

    BENCHMARK("Concatenate two small strings")
    {
        return String("blabla") + String("hehe");
    };
}
