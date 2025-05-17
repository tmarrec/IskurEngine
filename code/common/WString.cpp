// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "WString.h"

WString::WString() : m_Data(new wchar_t[1]{L'\0'}), m_Size(0), m_Capacity(0)
{
}

WString::WString(const wchar_t* s)
{
    m_Size = static_cast<u32>(wcslen(s));
    m_Capacity = m_Size;
    m_Data = new wchar_t[m_Capacity + 1];
    wmemcpy(m_Data, s, m_Size + 1);
}

WString::WString(const WString& o) : m_Data(new wchar_t[o.m_Capacity + 1]), m_Size(o.m_Size), m_Capacity(o.m_Capacity)
{
    wmemcpy(m_Data, o.m_Data, m_Size + 1);
}

WString::WString(WString&& o) noexcept : m_Data(o.m_Data), m_Size(o.m_Size), m_Capacity(o.m_Capacity)
{
    o.m_Data = nullptr;
    o.m_Size = 0;
    o.m_Capacity = 0;
}

WString::~WString()
{
    delete[] m_Data;
}

WString& WString::operator=(const WString& other)
{
    if (this != &other)
    {
        wchar_t* newData = new wchar_t[other.m_Capacity + 1];
        wmemcpy(newData, other.m_Data, other.m_Size + 1);
        delete[] m_Data;
        m_Data = newData;
        m_Size = other.m_Size;
        m_Capacity = other.m_Capacity;
    }
    return *this;
}

WString& WString::operator=(WString&& other) noexcept
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

bool WString::operator==(const WString& other) const
{
    if (m_Size != other.m_Size)
    {
        return false;
    }
    return wmemcmp(m_Data, other.m_Data, m_Size) == 0;
}

bool WString::operator!=(const WString& other) const
{
    return !(*this == other);
}

u32 WString::Size() const
{
    return m_Size;
}

bool WString::Empty() const
{
    return m_Size == 0;
}

const wchar_t* WString::Data() const
{
    return m_Data;
}

wchar_t& WString::operator[](u32 i)
{
    return m_Data[i];
}

const wchar_t& WString::operator[](u32 i) const
{
    return m_Data[i];
}

void WString::Reserve(u32 newCap)
{
    if (newCap <= m_Capacity)
    {
        return;
    }
    Grow(newCap);
}

WString& WString::Append(const wchar_t* s)
{
    const u32 addLen = static_cast<u32>(wcslen(s));
    const u32 req = m_Size + addLen;
    if (req > m_Capacity)
    {
        Grow(req);
    }

    wmemcpy(m_Data + m_Size, s, addLen + 1);
    m_Size = req;
    return *this;
}

WString& WString::operator+=(const wchar_t* s)
{
    return Append(s);
}

WString& WString::operator+=(const WString& other)
{
    return Append(other.Data());
}

void WString::Grow(u32 minCap)
{
    // Double capacity or satisfy minCap
    u32 newCap = m_Capacity ? m_Capacity * 2 : 1;
    if (newCap < minCap)
    {
        newCap = minCap;
    }

    wchar_t* newData = new wchar_t[newCap + 1];
    if (m_Data)
    {
        wmemcpy(newData, m_Data, m_Size + 1);
        delete[] m_Data;
    }
    m_Data = newData;
    m_Capacity = newCap;
}
