// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>

#include <format>
#include <utility>

#include "Types.h"

inline void IE_LogNoNewline(const String& msg)
{
    OutputDebugStringA(msg.c_str());
}

template <typename... Args> inline void IE_LogNoNewline(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogNoNewline(std::format(fmt, std::forward<Args>(args)...));
}

inline String IE_BuildLogPrefix(const char* tag)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format("[{:02}:{:02}:{:02}.{:03}][{}]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);
}

inline void IE_LogTagged(const char* tag, const String& msg)
{
    const String prefix = IE_BuildLogPrefix(tag);
    IE_LogNoNewline(prefix);
    if (!msg.empty() && msg.front() != '[')
    {
        IE_LogNoNewline(" ");
    }
    IE_LogNoNewline(msg);
    IE_LogNoNewline("\n");
}

template <typename... Args> inline void IE_LogTagged(const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged(tag, std::format(fmt, std::forward<Args>(args)...));
}

inline void IE_LogDebug(const String& msg)
{
    IE_LogTagged("DEBUG", msg);
}

template <typename... Args> inline void IE_LogDebug(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged("DEBUG", fmt, std::forward<Args>(args)...);
}

inline void IE_LogInfo(const String& msg)
{
    IE_LogTagged("INFO", msg);
}

template <typename... Args> inline void IE_LogInfo(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged("INFO", fmt, std::forward<Args>(args)...);
}

inline void IE_LogWarn(const String& msg)
{
    IE_LogTagged("WARN", msg);
}

template <typename... Args> inline void IE_LogWarn(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged("WARN", fmt, std::forward<Args>(args)...);
}

inline void IE_LogError(const String& msg)
{
    IE_LogTagged("ERROR", msg);
}

template <typename... Args> inline void IE_LogError(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged("ERROR", fmt, std::forward<Args>(args)...);
}

inline void IE_LogFatal(const String& msg)
{
    IE_LogTagged("FATAL", msg);
}

template <typename... Args> inline void IE_LogFatal(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogTagged("FATAL", fmt, std::forward<Args>(args)...);
}

