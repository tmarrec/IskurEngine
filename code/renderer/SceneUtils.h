// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

#include <filesystem>

namespace SceneUtils
{
struct SceneListEntry
{
    String name;
    u32 packVersion = 0;
    bool outOfDate = false;
};

bool EqualsIgnoreCaseAscii(const String& a, const String& b);
String SceneStemFromArg(String sceneArg);
Vector<SceneListEntry> EnumerateAvailableScenes();
String ResolveSceneNameFromList(const String& sceneArg, const Vector<SceneListEntry>& availableScenes);
const SceneListEntry* FindSceneInList(const String& sceneArg, const Vector<SceneListEntry>& availableScenes);
std::filesystem::path ResolveScenePackPath(const String& sceneArg);
} // namespace SceneUtils
