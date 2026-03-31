// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Asserts.h"

#include "Log.h"

namespace
{
String BuildHrMessage(HRESULT hr)
{
    LPSTR errorMessage = nullptr;
    const DWORD formatResult = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                              reinterpret_cast<LPSTR>(&errorMessage), 0, nullptr);
    if (formatResult == 0 || errorMessage == nullptr)
    {
        return "Unknown error";
    }

    String message = errorMessage;
    LocalFree(errorMessage);

    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back())))
    {
        message.pop_back();
    }

    return message.empty() ? String("Unknown error") : message;
}

void LogHrFailure(HRESULT hr, bool fatal)
{
    const String message = BuildHrMessage(hr);
    const unsigned long hrHex = static_cast<unsigned long>(hr);
    if (fatal)
    {
        IE_LogFatal("HRESULT 0x{:08X} ({})", hrHex, message);
    }
    else
    {
        IE_LogError("HRESULT 0x{:08X} ({})", hrHex, message);
    }
}
} // namespace

void IE_Assert(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

bool IE_Try(HRESULT hr)
{
    if (FAILED(hr))
    {
        LogHrFailure(hr, false);
        return false;
    }

    return true;
}

void IE_Check(HRESULT hr)
{
    if (FAILED(hr))
    {
        LogHrFailure(hr, true);
        abort();
    }
}
