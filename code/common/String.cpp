// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "String.h"

String::String() : m_Data(new char[1]{'\0'}), m_Size(0), m_Capacity(0)
{
}

String::String(const char* s)
{
    m_Size = static_cast<u32>(strlen(s));
    m_Capacity = m_Size;
    m_Data = new char[m_Capacity + 1];
    memcpy(m_Data, s, m_Size + 1);
}

String::String(const String& o) : m_Data(new char[o.m_Capacity + 1]), m_Size(o.m_Size), m_Capacity(o.m_Capacity)
{
    memcpy(m_Data, o.m_Data, m_Size + 1);
}

String::String(String&& o) noexcept : m_Data(o.m_Data), m_Size(o.m_Size), m_Capacity(o.m_Capacity)
{
    o.m_Data = nullptr;
    o.m_Size = 0;
    o.m_Capacity = 0;
}

String::~String()
{
    delete[] m_Data;
}

String& String::operator=(const String& other)
{
    if (this != &other)
    {
        char* newData = new char[other.m_Capacity + 1];
        memcpy(newData, other.m_Data, other.m_Size + 1);
        delete[] m_Data;
        m_Data = newData;
        m_Size = other.m_Size;
        m_Capacity = other.m_Capacity;
    }
    return *this;
}

String& String::operator=(String&& other) noexcept
{
    if (this != &other)
    {
        delete[] m_Data;
        m_Data = other.m_Data;
        m_Size = other.m_Size;
        m_Capacity = other.m_Capacity;
        other.m_Data = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
    }
    return *this;
}

bool String::operator==(const String& other) const
{
    if (m_Size != other.m_Size)
    {
        return false;
    }
    return memcmp(m_Data, other.m_Data, m_Size) == 0;
}

bool String::operator!=(const String& other) const
{
    return !(*this == other);
}

u32 String::Size() const
{
    return m_Size;
}

bool String::Empty() const
{
    return m_Size == 0;
}

const char* String::Data() const
{
    return m_Data;
}

char& String::operator[](u32 i)
{
    return m_Data[i];
}

const char& String::operator[](u32 i) const
{
    return m_Data[i];
}

void String::Reserve(u32 newCap)
{
    if (newCap <= m_Capacity)
        return;
    Grow(newCap);
}

String& String::Append(const char* s)
{
    const u32 addLen = static_cast<u32>(strlen(s));
    const u32 req = m_Size + addLen;
    if (req > m_Capacity)
    {
        Grow(req);
    }
    memcpy(m_Data + m_Size, s, addLen + 1);
    m_Size = req;
    return *this;
}

String& String::operator+=(const char* s)
{
    return Append(s);
}

String& String::operator+=(const String& other)
{
    return Append(other.Data());
}

void String::Grow(u32 minCap)
{
    u32 newCap = m_Capacity ? m_Capacity * 2 : 1;
    if (newCap < minCap)
    {
        newCap = minCap;
    }
    char* newData = new char[newCap + 1];
    if (m_Data)
    {
        memcpy(newData, m_Data, m_Size + 1);
        delete[] m_Data;
    }
    m_Data = newData;
    m_Capacity = newCap;
}
