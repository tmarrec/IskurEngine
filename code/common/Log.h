// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#define NOMINMAX
#include <Windows.h>

#include <format>

#include "Types.h"

static void IE_Log(const String& msg)
{
    OutputDebugStringA(msg.data());
    OutputDebugStringA("\n");
}

template <typename... Args> static void IE_Log(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string s = std::format(fmt, std::forward<Args>(args)...);
    IE_Log(String(s.c_str()));
}

static void IE_Error(const String& msg)
{
    OutputDebugStringA("Error: ");
    OutputDebugStringA(msg.data());
    OutputDebugStringA("\n");
}

template <typename... Args> static void IE_Error(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string s = std::format(fmt, std::forward<Args>(args)...);
    IE_Error(String(s.c_str()));
}