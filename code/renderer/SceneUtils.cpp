// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneUtils.h"

#include "common/IskurPackFormat.h"
#include "common/StringUtils.h"

#include <cstring>
#include <fstream>

namespace SceneUtils
{
namespace
{
namespace fs = std::filesystem;

bool MagicOk(const char magic[9])
{
    constexpr char mk[9] = {'I', 'S', 'K', 'U', 'R', 'P', 'A', 'C', 'K'};
    return std::memcmp(magic, mk, 9) == 0;
}

bool TryReadPackVersion(const fs::path& packPath, u32& outVersion)
{
    outVersion = 0;

    std::ifstream file(packPath, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    IEPack::PackHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header)))
    {
        return false;
    }

    if (!MagicOk(header.magic))
    {
        return false;
    }

    outVersion = header.version;
    return true;
}
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

Vector<SceneListEntry> EnumerateAvailableScenes()
{
    const fs::path baseDir = fs::path("data") / "scenes";
    Vector<SceneListEntry> scenes;

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

        SceneListEntry scene{};
        scene.name = p.stem().string();
        if (!TryReadPackVersion(p, scene.packVersion))
        {
            continue;
        }
        scene.outOfDate = scene.packVersion != IEPack::PACK_VERSION_LATEST;
        scenes.push_back(std::move(scene));
    }

    std::sort(scenes.begin(), scenes.end(), [](const SceneListEntry& a, const SceneListEntry& b) { return ToLowerAscii(a.name) < ToLowerAscii(b.name); });
    return scenes;
}

String ResolveSceneNameFromList(const String& sceneArg, const Vector<SceneListEntry>& availableScenes)
{
    const String stem = SceneStemFromArg(sceneArg);
    for (const SceneListEntry& scene : availableScenes)
    {
        if (EqualsIgnoreCaseAscii(scene.name, stem))
        {
            return scene.name;
        }
    }
    return stem;
}

const SceneListEntry* FindSceneInList(const String& sceneArg, const Vector<SceneListEntry>& availableScenes)
{
    const String stem = SceneStemFromArg(sceneArg);
    for (const SceneListEntry& scene : availableScenes)
    {
        if (EqualsIgnoreCaseAscii(scene.name, stem))
        {
            return &scene;
        }
    }

    return nullptr;
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
