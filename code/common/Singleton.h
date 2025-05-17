// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

template <typename T> class Singleton
{
  public:
    Singleton(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton& operator=(Singleton&&) = delete;

    static T& GetInstance()
    {
        static T instance;
        return instance;
    }

  protected:
    Singleton() = default;
    virtual ~Singleton() = default;
};
