// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>

#include <format>
#include <utility>

#include "Types.h"

enum class IE_LogLevel : u8
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

inline const char* IE_LogLevelToString(const IE_LogLevel level)
{
    switch (level)
    {
    case IE_LogLevel::Trace:
        return "TRACE";
    case IE_LogLevel::Debug:
        return "DEBUG";
    case IE_LogLevel::Info:
        return "INFO";
    case IE_LogLevel::Warn:
        return "WARN";
    case IE_LogLevel::Error:
        return "ERROR";
    case IE_LogLevel::Fatal:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

inline IE_LogLevel& IE_MinLogLevelStorage()
{
    static IE_LogLevel level =
#ifdef _DEBUG
        IE_LogLevel::Debug;
#else
        IE_LogLevel::Info;
#endif
    return level;
}

inline IE_LogLevel IE_GetMinLogLevel()
{
    return IE_MinLogLevelStorage();
}

inline void IE_SetMinLogLevel(const IE_LogLevel level)
{
    IE_MinLogLevelStorage() = level;
}

inline bool IE_ShouldLog(const IE_LogLevel level)
{
    return static_cast<u8>(level) >= static_cast<u8>(IE_MinLogLevelStorage());
}

inline void IE_LogNoNewline(const String& msg)
{
    OutputDebugStringA(msg.c_str());
}

template <typename... Args> inline void IE_LogNoNewline(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogNoNewline(std::format(fmt, std::forward<Args>(args)...));
}

inline String IE_BuildLogPrefix(const IE_LogLevel level)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format("[{:02}:{:02}:{:02}.{:03}][{}]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, IE_LogLevelToString(level));
}

inline void IE_LogMessage(const IE_LogLevel level, const String& msg)
{
    if (!IE_ShouldLog(level))
    {
        return;
    }

    const String prefix = IE_BuildLogPrefix(level);
    IE_LogNoNewline(prefix);
    if (!msg.empty() && msg.front() != '[')
    {
        IE_LogNoNewline(" ");
    }
    IE_LogNoNewline(msg);
    IE_LogNoNewline("\n");
}

template <typename... Args> inline void IE_LogMessage(const IE_LogLevel level, std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

inline void IE_LogTrace(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Trace, msg);
}

template <typename... Args> inline void IE_LogTrace(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Trace, fmt, std::forward<Args>(args)...);
}

inline void IE_LogDebug(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Debug, msg);
}

template <typename... Args> inline void IE_LogDebug(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

inline void IE_LogInfo(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Info, msg);
}

template <typename... Args> inline void IE_LogInfo(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Info, fmt, std::forward<Args>(args)...);
}

inline void IE_LogWarn(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Warn, msg);
}

template <typename... Args> inline void IE_LogWarn(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

inline void IE_LogError(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Error, msg);
}

template <typename... Args> inline void IE_LogError(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Error, fmt, std::forward<Args>(args)...);
}

inline void IE_LogFatal(const String& msg)
{
    IE_LogMessage(IE_LogLevel::Fatal, msg);
}

template <typename... Args> inline void IE_LogFatal(std::format_string<Args...> fmt, Args&&... args)
{
    IE_LogMessage(IE_LogLevel::Fatal, fmt, std::forward<Args>(args)...);
}

