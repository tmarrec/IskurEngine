// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/MathUtils.h"
#include "common/Types.h"

enum class RayTracingResolution : u8
{
    Full = 0,        // Full resolution (x, y)
    FullX_HalfY = 1, // Full resolution in x, half resolution in y
    Half = 2,        // Both x and y at half resolution
    Quarter = 3      // Both x and y at quarter resolution
};

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
    f32 ambientStrength = 0.005f;
    f32 shadowMinVisibility = 0.0f;
    f32 rtIndirectDiffuseStrength = 1.0f;

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
    RayTracingResolution rtShadowsResolution = RayTracingResolution::FullX_HalfY;
    bool rtSpecularEnabled = true;
    RayTracingResolution rtSpecularResolution = RayTracingResolution::FullX_HalfY;
    f32 rtSpecularStrength = 1.0f;
    u32 rtSpecularSppMin = 1;
    u32 rtSpecularSppMax = 1;

    u32 pathTraceSpp = 1;
    u32 pathTraceBounceCount = 2;
    bool radianceCacheTrilinear = true;
    bool radianceCacheSoftNormalInterpolation = true;
    f32 radianceCacheSoftNormalMinDot = 0.50f;
    u32 radianceCacheTrilinearMinCornerSamples = 128;
    u32 radianceCacheTrilinearMinHits = 2;
    u32 radianceCacheTrilinearPresentMinSamples = 64;
    u32 radianceCacheNormalBinRes = 8;
    u32 radianceCacheMinExtraSppCount = 32u;
    u32 radianceCacheMaxProbes = 64;
    u32 radianceCacheMaxSamples = 131072u;
    f32 radianceCacheCellSize = 0.10f;

    bool testMoveInstances = false;
    f32 testMoveInstancesAmplitude = 0.05f;
    f32 testMoveInstancesSpeed = 0.05f;
    u32 testMoveInstancesCount = 64;
    bool debugMeshletColor = false;
    u32 lightingDebugMode = 0; // 0=None, 1=Indirect Diffuse, 2=Normal
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
