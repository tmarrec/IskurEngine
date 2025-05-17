// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Types.h"

class String
{
  public:
    String();
    String(const char* s);
    String(const String& o);
    String(String&& o) noexcept;

    ~String();

    String& operator=(const String& other);
    String& operator=(String&& other) noexcept;

    bool operator==(const String& other) const;
    bool operator!=(const String& other) const;

    u32 Size() const;
    bool Empty() const;

    const char* Data() const;

    char& operator[](u32 i);
    const char& operator[](u32 i) const;

    void Reserve(u32 newCap);
    String& Append(const char* s);
    String& operator+=(const char* s);
    String& operator+=(const String& other);

  private:
    void Grow(u32 minCap);

    char* m_Data;
    u32 m_Size;
    u32 m_Capacity;
};

inline String operator+(String lhs, const String& rhs)
{
    lhs += rhs;
    return lhs;
}

inline String operator+(String lhs, const char* rhs)
{
    lhs += rhs;
    return lhs;
}

inline String operator+(const char* lhs, const String& rhs)
{
    String tmp(lhs);
    tmp += rhs;
    return tmp;
}