// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

using u32 = std::uint32_t;

template <typename T> class Vector
{
  public:
    Vector() : m_Data(nullptr), m_Size(0), m_Capacity(0)
    {
    }

    explicit Vector(u32 size) : m_Data(nullptr), m_Size(size), m_Capacity(size)
    {
        m_Data = new T[m_Capacity];
    }

    Vector(const Vector& other)
    {
        m_Size = other.m_Size;
        m_Capacity = other.m_Capacity;
        m_Data = new T[m_Capacity];
        for (u32 i = 0; i < m_Size; ++i)
        {
            m_Data[i] = other.m_Data[i];
        }
    }

    Vector(std::initializer_list<T> initializerList) : m_Size(initializerList.size()), m_Capacity(initializerList.size())
    {
        m_Data = new T[m_Capacity];
        u32 i = 0;
        for (const T& value : initializerList)
        {
            m_Data[i++] = value;
        }
    }

    Vector& operator=(const Vector& other)
    {
        if (this != &other)
        {
            delete[] m_Data;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            m_Data = new T[m_Capacity];
            for (u32 i = 0; i < m_Size; ++i)
            {
                m_Data[i] = other.m_Data[i];
            }
        }
        return *this;
    }

    Vector(Vector&& other) noexcept
    {
        m_Data = other.m_Data;
        m_Size = other.m_Size;
        m_Capacity = other.m_Capacity;
        other.m_Data = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
    }

    Vector& operator=(Vector&& other) noexcept
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

    ~Vector()
    {
        delete[] m_Data;
    }

    u32 Size() const
    {
        return m_Size;
    }

    u32 ByteSize() const
    {
        return m_Size * sizeof(T);
    }

    u32 Capacity() const
    {
        return m_Capacity;
    }

    bool Empty() const
    {
        return m_Size == 0;
    }

    T& operator[](u32 index)
    {
        return m_Data[index];
    }

    const T& operator[](u32 index) const
    {
        return m_Data[index];
    }

    T& Back()
    {
        return m_Data[m_Size - 1];
    }

    const T& Back() const
    {
        return m_Data[m_Size - 1];
    }

    void Add(const T& value)
    {
        if (m_Size == m_Capacity)
        {
            Reallocate(m_Capacity == 0 ? 1 : m_Capacity * 2);
        }
        m_Data[m_Size++] = value;
    }

    void Resize(u32 newSize)
    {
        if (newSize > m_Capacity)
        {
            Reallocate(newSize);
        }
        m_Size = newSize;
    }

    void Clear()
    {
        m_Size = 0;
    }

    T* Find(const T& value)
    {
        for (T* it = begin(); it != end(); ++it)
        {
            if (*it == value)
            {
                return it;
            }
        }
        return nullptr;
    }

    const T* Find(const T& value) const
    {
        for (const T* it = begin(); it != end(); ++it)
        {
            if (*it == value)
            {
                return it;
            }
        }
        return nullptr;
    }

    T* Data()
    {
        return m_Data;
    }

    const T* Data() const
    {
        return m_Data;
    }

    T* begin()
    {
        return m_Data;
    }

    const T* begin() const
    {
        return m_Data;
    }

    T* end()
    {
        return m_Data + m_Size;
    }

    const T* end() const
    {
        return m_Data + m_Size;
    }

  private:
    void Reallocate(u32 newCapacity)
    {
        T* newData = new T[newCapacity];
        for (u32 i = 0; i < m_Size; ++i)
        {
            newData[i] = m_Data[i];
        }
        delete[] m_Data;
        m_Data = newData;
        m_Capacity = newCapacity;
    }

    T* m_Data;
    u32 m_Size;
    u32 m_Capacity;
};