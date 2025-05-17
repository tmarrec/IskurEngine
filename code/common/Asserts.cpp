// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Asserts.h"

#include "Log.h"

void IE_Assert(bool assert)
{
    if (!assert)
    {
        abort();
    }
}

void IE_Check(HRESULT hr)
{
    LPSTR errorMessage = nullptr;
    if (FAILED(hr))
    {
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&errorMessage), 0, nullptr);
        if (errorMessage)
        {
            IE_Error("0x{:X} - {}\n", hr, errorMessage);
            LocalFree(errorMessage);
        }
        abort();
    }
}
