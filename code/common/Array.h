// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

using u32 = std::uint32_t;

template <typename T, u32 N> class Array
{
  public:
    Array() = default;

    Array(std::initializer_list<T> initializerList)
    {
        u32 i = 0;
        for (const T& value : initializerList)
        {
            m_Data[i++] = value;
        }
    }

    T& operator[](u32 index)
    {
        return m_Data[index];
    }

    const T& operator[](u32 index) const
    {
        return m_Data[index];
    }

    static u32 Size()
    {
        return N;
    }

    T& Front()
    {
        return m_Data[0];
    }

    const T& Front() const
    {
        return m_Data[0];
    }

    T& Back()
    {
        return m_Data[N - 1];
    }

    const T& Back() const
    {
        return m_Data[N - 1];
    }

    T* begin()
    {
        return &m_Data[0];
    }

    const T* begin() const
    {
        return &m_Data[0];
    }

    T* end()
    {
        return &m_Data[N];
    }

    const T* end() const
    {
        return &m_Data[N];
    }

    T* Data()
    {
        return m_Data;
    }

    const T* Data() const
    {
        return m_Data;
    }

  private:
    T m_Data[N];
};