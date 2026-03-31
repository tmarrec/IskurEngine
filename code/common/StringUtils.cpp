// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "StringUtils.h"

#include <cctype>

String ToLowerAscii(String s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool EqualsIgnoreCaseAscii(const String& a, const String& b)
{
    return ToLowerAscii(a) == ToLowerAscii(b);
}
