// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/MathUtils.h"
#include "common/Types.h"

struct RuntimeSettings
{
    f32 timingAverageWindowMs = 2000.0f;

    f32 toneMappingExposureCompensationEV = 0.0f;
    f32 toneMappingContrast = 1.015f;
    f32 toneMappingSaturation = 1.150f;
    bool bloomEnabled = true;
    f32 bloomIntensity = 0.07f;
    f32 bloomThreshold = 1.0f;
    f32 bloomSoftKnee = 0.5f;
    f32 dlssJitterScale = 1.0f;

    f32 cameraFov = 58.71550709f;
    f32 cameraFrustumCullingFov = 58.71550709f;
    bool cpuFrustumCulling = true;
    bool gpuFrustumCulling = true;
    bool gpuBackfaceCulling = true;

    f32 sunAzimuth = IE_ToRadians(210.0f);
    f32 sunElevation = IE_ToRadians(240.0f);
    f32 sunIntensity = 1.0f;

    f32 skyIntensity = 5.0f;
    f32 shadowMinVisibility = 0.0f;
    f32 specularShadowMinVisibility = 0.0f;

    f32 autoExposureTargetPct = 0.65f;
    f32 autoExposureLowReject = 0.01f;
    f32 autoExposureHighReject = 0.98f;
    f32 autoExposureKey = 0.18f;
    f32 autoExposureMinLogLum = -8.0f;
    f32 autoExposureMaxLogLum = 3.5f;
    f32 autoExposureClampMin = 1.0f / 5.0f;
    f32 autoExposureClampMax = 32.0f;
    f32 autoExposureTauBright = 0.14f;
    f32 autoExposureTauDark = 1.0f;

    bool rtShadowsEnabled = true;
    bool rtOmmsEnabled = true;
    bool rtUse2StateOmmRays = true;
    bool rtSerEnabled = true;

    u32 rtPathTraceSpp = 1;
    u32 rtMaxBounces = 12;

    bool testMoveInstances = false;
    f32 testMoveInstancesAmplitude = 0.05f;
    f32 testMoveInstancesSpeed = 0.05f;
    u32 testMoveInstancesCount = 64;
    bool debugMeshletColor = false;
    u32 lightingDebugMode = 0; // 0=None, 1=Surface Normals
};

struct RuntimeStats
{
    u32 cpuFrustumCullTotalInstances = 0;
    u32 cpuFrustumCullRasterSubmitted = 0;
    u32 cpuFrustumCullRasterCulled = 0;
    bool shadersCompilationSuccess = true;
};

extern RuntimeSettings g_Settings;
extern RuntimeStats g_Stats;
