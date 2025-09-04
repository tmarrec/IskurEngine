// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <DirectXMath.h>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

using namespace DirectX;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;  // std::float32_t when C++23 is available
using f64 = double; // std::float64_t when C++23 is available

template <class T, class Alloc = std::allocator<T>> using Vector = std::vector<T, Alloc>;
template <class T, std::size_t N> using Array = std::array<T, N>;
using String = std::string;
using WString = std::wstring;

template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

template <typename T> using SharedPtr = std::shared_ptr<T>;
template <typename T> using WeakPtr = std::weak_ptr<T>;
template <typename T, typename Deleter = std::default_delete<T>> using UniquePtr = std::unique_ptr<T, Deleter>;

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

template <typename T> UniquePtr<T> IE_MakeUniquePtr()
{
    return std::make_unique<T>();
}

template <typename T, class... Types> UniquePtr<T> IE_MakeUniquePtr(Types... args)
{
    return std::make_unique<T>(args...);
}

template <typename T> WeakPtr<T> IE_MakeWeakPtr(const SharedPtr<T>& sp)
{
    return WeakPtr<T>(sp);
}
