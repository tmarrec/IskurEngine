// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <string>

#include "Types.h"

std::wstring Utf8ToWide(const String& text);
String WideToUtf8(const wchar_t* text);
