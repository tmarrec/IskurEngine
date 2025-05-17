// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <wrl/client.h>

template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

template <typename T> using SharedPtr = std::shared_ptr<T>;
template <typename T> SharedPtr<T> IE_MakeSharedPtr()
{
    return std::make_shared<T>();
}
template <typename T> SharedPtr<T> IE_MakeSharedPtr(T& ref)
{
    return std::make_shared<T>(ref);
}
template <typename T, class... Types> SharedPtr<T> IE_MakeSharedPtr(Types... args)
{
    return std::make_shared<T>(args...);
}

template <typename T, typename Deleter = std::default_delete<T>> using UniquePtr = std::unique_ptr<T, Deleter>;
template <typename T, typename Deleter = std::default_delete<T>> UniquePtr<T> IE_MakeUniquePtr()
{
    return std::make_unique<T, Deleter>();
}
template <typename T, typename Deleter = std::default_delete<T>> UniquePtr<T, Deleter> IE_MakeUniquePtr(T& ref)
{
    return std::make_unique<T, Deleter>(ref);
}
template <typename T, typename Deleter = std::default_delete<T>> UniquePtr<T, Deleter> IE_MakeUniquePtr(T* ref)
{
    return std::make_unique<T, Deleter>(ref);
}
template <typename T, class... Types> UniquePtr<T> IE_MakeUniquePtr(Types... args)
{
    return std::make_unique<T>(args...);
}
