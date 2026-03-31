// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

struct Environment
{
    XMFLOAT3 sunDir = {0.3f, 1.f, 0.75f};

    struct SkySettings
    {
        struct AtmosphereSettings
        {
            // Coefficients are in 1/km style units used by the procedural atmosphere integrator.
            XMFLOAT3 rayleighScattering = {0.0058f, 0.0135f, 0.0331f};
            f32 rayleighScaleHeightKm = 8.0f;

            XMFLOAT3 mieScattering = {0.021f, 0.021f, 0.021f};
            f32 mieScaleHeightKm = 1.2f;

            f32 mieG = 0.76f;
            f32 atmosphereThicknessKm = 80.0f;
            f32 sunIntensityScale = 4.0f;

            // Ozone absorption (approximate, Earth-like defaults).
            XMFLOAT3 ozoneAbsorption = {0.00065f, 0.001881f, 0.000085f};
            f32 ozoneLayerCenterKm = 25.0f;
            f32 ozoneLayerWidthKm = 15.0f;

            // Heuristic second-order bounce term for brighter, more physical-looking skylight.
            f32 multiScatteringStrength = 0.35f;
        } atmosphere{};

        XMFLOAT3 sunColor = {1.0f, 0.95f, 0.85f};
        f32 sunDiskAngleDeg = 0.12f;

        f32 sunGlowPower = 64.0f;
        f32 sunGlowIntensity = 0.05f;
        f32 sunDiskIntensityScale = 3.0f;
    } sky{};
};

class Environments
{
  public:
    struct EnvironmentPreset
    {
        String name;
        Environment environment{};
        f32 sunAzimuthDeg = 210.0f;
        f32 sunElevationDeg = 240.0f;
        f32 sunIntensity = 1.0f;
        f32 skyIntensity = 5.0f;
        f32 ambientStrength = 0.005f;
        f32 shadowMinVisibility = 0.0f;
        f32 rtIndirectDiffuseStrength = 1.0f;
        f32 exposureCompensationEV = 0.0f;
    };

    void Load();

    const Vector<String>& GetEnvironmentNames() const;
    const String& GetCurrentEnvironmentName() const;
    i32 GetCurrentEnvironmentIndex() const;
    bool SetCurrentEnvironmentIndex(i32 index);
    bool SetCurrentEnvironmentByName(const String& name);

    const Environment& GetCurrentEnvironment() const;
    Environment& GetCurrentEnvironment();

  private:
    void ApplyPreset(const EnvironmentPreset& preset);

    Vector<EnvironmentPreset> m_Presets;
    Vector<String> m_PresetNames;
    Environment m_CurrentEnvironment;
    String m_CurrentEnvironmentName = "Default";
    i32 m_CurrentEnvironmentIndex = -1;
};

