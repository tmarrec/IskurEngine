// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DISABLE_EXCEPTIONS
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "../common/WString.h"

TEST_CASE("WString basic operations", "[WString]")
{
    SECTION("Default constructor")
    {
        WString s;
        REQUIRE(s.Empty());
        REQUIRE(s.Size() == 0u);
        REQUIRE(s.Data() != nullptr);
        REQUIRE(s.Data()[0] == L'\0');
    }

    SECTION("C-string constructor and indexing")
    {
        WString s(L"hello");
        REQUIRE(!s.Empty());
        REQUIRE(s.Size() == 5u);
        REQUIRE(std::wstring(s.Data()) == L"hello");
        REQUIRE(s[0] == L'h');
        REQUIRE(s[1] == L'e');
        REQUIRE(s[4] == L'o');
    }

    SECTION("Copy constructor")
    {
        WString original(L"test");
        WString copy(original);
        REQUIRE(copy == original);
        REQUIRE(copy.Size() == original.Size());
        REQUIRE(std::wstring(copy.Data()) == std::wstring(original.Data()));
    }

    SECTION("Move constructor")
    {
        WString src(L"move");
        WString dst(std::move(src));
        REQUIRE(dst.Size() == 4u);
        REQUIRE(std::wstring(dst.Data()) == L"move");
        REQUIRE(src.Empty());
        REQUIRE(src.Data() == nullptr);
    }

    SECTION("Copy assignment")
    {
        WString a(L"one"), b(L"two");
        a = b;
        REQUIRE(a == b);
        REQUIRE(std::wstring(a.Data()) == L"two");
    }

    SECTION("Move assignment")
    {
        WString a(L"foo"), b(L"bar");
        a = std::move(b);
        REQUIRE(a.Size() == 3u);
        REQUIRE(std::wstring(a.Data()) == L"bar");
        REQUIRE(b.Empty());
        REQUIRE(b.Data() == nullptr);
    }

    SECTION("Append and += operators")
    {
        WString s(L"hello");
        s.Append(L" world");
        REQUIRE(std::wstring(s.Data()) == L"hello world");
        REQUIRE(s.Size() == 11u);

        s = WString(L"foo");
        s += L"bar";
        REQUIRE(std::wstring(s.Data()) == L"foobar");
        REQUIRE(s.Size() == 6u);

        WString part(L"baz");
        s += part;
        REQUIRE(std::wstring(s.Data()) == L"foobarbaz");
        REQUIRE(s.Size() == 9u);
    }

    SECTION("Reserve behavior")
    {
        WString s(L"abc");
        s.Reserve(2);
        REQUIRE(std::wstring(s.Data()) == L"abc");
        REQUIRE(s.Size() == 3u);

        s.Reserve(10);
        s.Append(L"defgh");
        REQUIRE(std::wstring(s.Data()) == L"abcdefgh");
        REQUIRE(s.Size() == 8u);
    }

    SECTION("Operator+ concatenation")
    {
        WString a(L"blabla");
        WString b(L"hehe");
        WString c = a + b;
        REQUIRE(c == WString(L"blablahehe"));
        REQUIRE(a == WString(L"blabla"));
        REQUIRE(b == WString(L"hehe"));
    }
}

TEST_CASE("WString performance benchmarks", "[WString][benchmark]")
{
    BENCHMARK("Default construction")
    {
        return WString();
    };

    BENCHMARK("Construction from literal")
    {
        return WString(L"benchmark");
    };

    BENCHMARK("Append single chars 100 times")
    {
        WString s;
        for (u8 i = 0; i < 100; ++i)
        {
            s.Append(L"x");
        }
        return s;
    };

    BENCHMARK("Reserve then append 100 chars")
    {
        WString s;
        s.Reserve(1000);
        for (u8 i = 0; i < 100; ++i)
        {
            s.Append(L"y");
        }
        return s;
    };

    BENCHMARK("Copy large string")
    {
        WString big;
        for (u16 i = 0; i < 1000; ++i)
        {
            big.Append(L"z");
        }
        return WString(big);
    };

    BENCHMARK("Move large string")
    {
        WString big;
        for (u16 i = 0; i < 1000; ++i)
        {
            big.Append(L"z");
        }
        return WString(std::move(big));
    };

    BENCHMARK("Concatenate two small WStrings")
    {
        return WString(L"blabla") + WString(L"hehe");
    };
}
