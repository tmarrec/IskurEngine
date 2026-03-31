// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

#include <filesystem>

namespace SceneUtils
{
bool EqualsIgnoreCaseAscii(const String& a, const String& b);
String SceneStemFromArg(String sceneArg);
Vector<String> EnumerateAvailableScenes();
String ResolveSceneNameFromList(const String& sceneArg, const Vector<String>& availableScenes);
std::filesystem::path ResolveScenePackPath(const String& sceneArg);
} // namespace SceneUtils
