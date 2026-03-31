// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneUtils.h"

#include "common/IskurPackFormat.h"
#include "common/StringUtils.h"

namespace SceneUtils
{
namespace
{
namespace fs = std::filesystem;
} // namespace

bool EqualsIgnoreCaseAscii(const String& a, const String& b)
{
    return ::EqualsIgnoreCaseAscii(a, b);
}

String SceneStemFromArg(String sceneArg)
{
    const String lower = ToLowerAscii(sceneArg);

    if (sceneArg.size() >= 4)
    {
        if (lower.ends_with(".glb"))
        {
            return sceneArg.substr(0, sceneArg.size() - 4);
        }
    }

    constexpr size_t packExtLen = sizeof(IEPack::PACK_FILE_EXTENSION) - 1;
    if (sceneArg.size() >= packExtLen && lower.ends_with(IEPack::PACK_FILE_EXTENSION))
    {
        return sceneArg.substr(0, sceneArg.size() - packExtLen);
    }

    return sceneArg;
}

Vector<String> EnumerateAvailableScenes()
{
    const fs::path baseDir = fs::path("data") / "scenes";
    Vector<String> scenes;

    if (!fs::exists(baseDir) || !fs::is_directory(baseDir))
    {
        return scenes;
    }
    for (const fs::directory_entry& de : fs::directory_iterator(baseDir))
    {
        if (!de.is_regular_file())
        {
            continue;
        }

        const fs::path p = de.path();
        if (!EqualsIgnoreCaseAscii(p.extension().string(), IEPack::PACK_FILE_EXTENSION))
        {
            continue;
        }
        scenes.push_back(p.stem().string());
    }

    std::sort(scenes.begin(), scenes.end(), [](const String& a, const String& b) { return ToLowerAscii(a) < ToLowerAscii(b); });
    return scenes;
}

String ResolveSceneNameFromList(const String& sceneArg, const Vector<String>& availableScenes)
{
    const String stem = SceneStemFromArg(sceneArg);
    for (const String& scene : availableScenes)
    {
        if (EqualsIgnoreCaseAscii(scene, stem))
        {
            return scene;
        }
    }
    return stem;
}

std::filesystem::path ResolveScenePackPath(const String& sceneArg)
{
    const fs::path baseDir = fs::path("data") / "scenes";
    const String stem = SceneStemFromArg(sceneArg);
    const fs::path direct = baseDir / (stem + IEPack::PACK_FILE_EXTENSION);
    if (fs::exists(direct) && fs::is_regular_file(direct))
    {
        return direct;
    }

    if (!fs::exists(baseDir) || !fs::is_directory(baseDir))
    {
        return direct;
    }

    const String stemLower = ToLowerAscii(stem);
    for (const fs::directory_entry& de : fs::directory_iterator(baseDir))
    {
        if (!de.is_regular_file())
        {
            continue;
        }

        const fs::path p = de.path();
        if (!EqualsIgnoreCaseAscii(p.extension().string(), IEPack::PACK_FILE_EXTENSION))
        {
            continue;
        }

        if (EqualsIgnoreCaseAscii(p.stem().string(), stemLower))
        {
            return p;
        }
    }

    return direct;
}
} // namespace SceneUtils
