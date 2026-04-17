// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Environments.h"

#include "RuntimeState.h"
#include "common/StringUtils.h"
#include <sstream>

namespace
{
constexpr const char* kEnvironmentPresetsPath = "data/environment_presets.txt";

Environments::EnvironmentPreset MakeDefaultPreset()
{
    Environments::EnvironmentPreset preset{};
    preset.name = "Default";
    preset.environment = Environment{};
    return preset;
}

XMFLOAT3 ComputeSunDirection(const f32 sunAzimuthDeg, const f32 sunElevationDeg)
{
    const f32 sunAzimuth = IE_ToRadians(sunAzimuthDeg);
    const f32 sunElevation = IE_ToRadians(sunElevationDeg);
    const f32 sinElevation = IE_Sinf(sunElevation);
    const f32 cosElevation = IE_Cosf(sunElevation);
    XMVECTOR sun = XMVectorSet(cosElevation * IE_Cosf(sunAzimuth), sinElevation, cosElevation * IE_Sinf(sunAzimuth), 0.0f);
    sun = XMVector3Normalize(sun);

    XMFLOAT3 sunDir{};
    XMStoreFloat3(&sunDir, sun);
    return sunDir;
}

bool TryParsePresetLine(const String& line, Environments::EnvironmentPreset& outPreset)
{
    if (line.empty() || line[0] == '#')
    {
        return false;
    }

    Environment env{};
    std::istringstream stream(line);
    if (!(stream >> outPreset.name >> outPreset.sunAzimuthDeg >> outPreset.sunElevationDeg >> outPreset.sunIntensity >> outPreset.skyIntensity >>
          outPreset.shadowMinVisibility >> outPreset.specularShadowMinVisibility >> outPreset.exposureCompensationEV >> env.sky.sunColor.x >>
          env.sky.sunColor.y >> env.sky.sunColor.z >> env.sky.sunDiskAngleDeg >> env.sky.sunGlowPower >> env.sky.sunGlowIntensity >> env.sky.sunDiskIntensityScale >>
          env.sky.atmosphere.rayleighScattering.x >> env.sky.atmosphere.rayleighScattering.y >> env.sky.atmosphere.rayleighScattering.z >> env.sky.atmosphere.rayleighScaleHeightKm >>
          env.sky.atmosphere.mieScattering.x >> env.sky.atmosphere.mieScattering.y >> env.sky.atmosphere.mieScattering.z >> env.sky.atmosphere.mieScaleHeightKm >> env.sky.atmosphere.mieG >>
          env.sky.atmosphere.atmosphereThicknessKm >> env.sky.atmosphere.sunIntensityScale >> env.sky.atmosphere.ozoneAbsorption.x >> env.sky.atmosphere.ozoneAbsorption.y >>
          env.sky.atmosphere.ozoneAbsorption.z >> env.sky.atmosphere.ozoneLayerCenterKm >> env.sky.atmosphere.ozoneLayerWidthKm >> env.sky.atmosphere.multiScatteringStrength))
    {
        return false;
    }

    outPreset.environment = env;
    outPreset.environment.sunDir = ComputeSunDirection(outPreset.sunAzimuthDeg, outPreset.sunElevationDeg);
    return true;
}
} // namespace

void Environments::Load()
{
    m_Presets.clear();
    m_PresetNames.clear();
    m_CurrentEnvironmentIndex = -1;
    m_CurrentEnvironmentName = "Default";
    m_CurrentEnvironment = Environment{};

    std::ifstream file(kEnvironmentPresetsPath);
    if (!file.is_open())
    {
        IE_LogWarn("Failed to open '{}', using built-in default environment.", kEnvironmentPresetsPath);
    }
    else
    {
        String line;
        u32 lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            EnvironmentPreset preset{};
            if (!TryParsePresetLine(line, preset))
            {
                if (!line.empty() && line[0] != '#')
                {
                    IE_LogWarn("Failed to parse environment preset line {} from '{}'", lineNumber, kEnvironmentPresetsPath);
                }
                continue;
            }

            m_PresetNames.push_back(preset.name);
            m_Presets.push_back(preset);
        }
    }

    if (m_Presets.empty())
    {
        const EnvironmentPreset defaultPreset = MakeDefaultPreset();
        m_PresetNames.push_back(defaultPreset.name);
        m_Presets.push_back(defaultPreset);
    }

    i32 defaultPresetIndex = 0;
    for (i32 i = 0; i < static_cast<i32>(m_Presets.size()); ++i)
    {
        if (m_Presets[static_cast<size_t>(i)].name == "Afternoon")
        {
            defaultPresetIndex = i;
            break;
        }
    }

    SetCurrentEnvironmentIndex(defaultPresetIndex);
    IE_LogInfo("Loaded {} environment preset(s) from '{}'", m_Presets.size(), kEnvironmentPresetsPath);
}

const Vector<String>& Environments::GetEnvironmentNames() const
{
    return m_PresetNames;
}

const String& Environments::GetCurrentEnvironmentName() const
{
    return m_CurrentEnvironmentName;
}

i32 Environments::GetCurrentEnvironmentIndex() const
{
    return m_CurrentEnvironmentIndex;
}

bool Environments::SetCurrentEnvironmentIndex(const i32 index)
{
    if (index < 0 || index >= static_cast<i32>(m_Presets.size()))
    {
        return false;
    }

    ApplyPreset(m_Presets[static_cast<size_t>(index)]);
    m_CurrentEnvironmentIndex = index;
    return true;
}

bool Environments::SetCurrentEnvironmentByName(const String& name)
{
    for (i32 i = 0; i < static_cast<i32>(m_Presets.size()); ++i)
    {
        if (EqualsIgnoreCaseAscii(m_Presets[static_cast<size_t>(i)].name, name))
        {
            return SetCurrentEnvironmentIndex(i);
        }
    }

    return false;
}

const Environment& Environments::GetCurrentEnvironment() const
{
    return m_CurrentEnvironment;
}

Environment& Environments::GetCurrentEnvironment()
{
    return m_CurrentEnvironment;
}

void Environments::ApplyPreset(const EnvironmentPreset& preset)
{
    m_CurrentEnvironment = preset.environment;
    m_CurrentEnvironmentName = preset.name;

    g_Settings.sunAzimuth = IE_ToRadians(preset.sunAzimuthDeg);
    g_Settings.sunElevation = IE_ToRadians(preset.sunElevationDeg);
    g_Settings.sunIntensity = preset.sunIntensity;
    g_Settings.skyIntensity = preset.skyIntensity;
    g_Settings.shadowMinVisibility = preset.shadowMinVisibility;
    g_Settings.specularShadowMinVisibility = preset.specularShadowMinVisibility;
    g_Settings.toneMappingExposureCompensationEV = preset.exposureCompensationEV;
}
