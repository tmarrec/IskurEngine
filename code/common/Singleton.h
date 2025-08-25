// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

template <typename T> class Singleton
{
  public:
    static T& GetInstance()
    {
        auto& p = Holder();
        if (!p)
        {
            p.reset(new T());
        }
        return *p;
    }

    static void DestroyInstance()
    {
        Holder().reset();
    }

  protected:
    static UniquePtr<T>& Holder()
    {
        static UniquePtr<T> p;
        return p;
    }
};
