// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "ToUtf8.h"

#define NOMINMAX
#include <Windows.h>

String IE_ToUtf8(const WString& ws)
{
    if (ws.empty())
    {
        return {};
    }

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<i32>(ws.size()), nullptr, 0, nullptr, nullptr);
    String result(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<i32>(ws.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}