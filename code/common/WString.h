// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Types.h"

class WString
{
  public:
    WString();
    WString(const wchar_t* s);
    WString(const WString& o);
    WString(WString&& o) noexcept;

    ~WString();

    WString& operator=(const WString& other);
    WString& operator=(WString&& other) noexcept;

    bool operator==(const WString& other) const;
    bool operator!=(const WString& other) const;

    u32 Size() const;
    bool Empty() const;

    const wchar_t* Data() const;

    wchar_t& operator[](u32 i);
    const wchar_t& operator[](u32 i) const;

    void Reserve(u32 newCap);
    WString& Append(const wchar_t* s);
    WString& operator+=(const wchar_t* s);
    WString& operator+=(const WString& other);

  private:
    void Grow(u32 minCap);

    wchar_t* m_Data;
    u32 m_Size;
    u32 m_Capacity;
};

inline WString operator+(WString lhs, const WString& rhs)
{
    lhs += rhs;
    return lhs;
}

inline WString operator+(WString lhs, const wchar_t* rhs)
{
    lhs += rhs;
    return lhs;
}

inline WString operator+(const wchar_t* lhs, const WString& rhs)
{
    WString tmp(lhs);
    tmp += rhs;
    return tmp;
}