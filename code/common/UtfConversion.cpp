// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "UtfConversion.h"

std::wstring Utf8ToWide(const String& text)
{
    if (text.empty())
    {
        return {};
    }

    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), sizeNeeded);
    return result;
}

String WideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
    {
        return {};
    }

    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 1)
    {
        return {};
    }

    String result(static_cast<size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), sizeNeeded, nullptr, nullptr);
    result.pop_back();
    return result;
}
