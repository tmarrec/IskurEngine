// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

enum class RayTracingResolution : u8
{
    Full = 0,        // Full resolution (x, y)
    FullX_HalfY = 1, // Full resolution in x, half resolution in y
    Half = 2,        // Both x and y at half resolution
    Quarter = 3      // Both x and y at quarter resolution
};

enum class EnvironmentFile : u8
{
    AutumnField = 0,
    BelfastSunset = 1,
    PartlyCloudy = 2,
    OvercastSoil = 3,

    Count = 4,
};

// Timing
extern f32 g_Timing_AverageWindowMs;

// Tone mapping
extern f32 g_ToneMapping_WhitePoint;
extern f32 g_ToneMapping_Contrast;
extern f32 g_ToneMapping_Saturation;

// Camera
extern f32 g_Camera_FOV;
extern f32 g_Camera_FrustumCullingFOV;

// Sun
extern f32 g_Sun_Azimuth;   // radians
extern f32 g_Sun_Elevation; // radians
extern f32 g_Sun_Intensity;

// IBL
extern f32 g_IBL_SpecularIntensity;
extern f32 g_IBL_SkyIntensity;

// Auto exposure
extern f32 g_AutoExposure_TargetPct;
extern f32 g_AutoExposure_LowReject;
extern f32 g_AutoExposure_HighReject;
extern f32 g_AutoExposure_Key;
extern f32 g_AutoExposure_MinLogLum;
extern f32 g_AutoExposure_MaxLogLum;
extern f32 g_AutoExposure_ClampMin;
extern f32 g_AutoExposure_ClampMax;
extern f32 g_AutoExposure_TauBright;
extern f32 g_AutoExposure_TauDark;

// RT shadows
extern bool g_RTShadows_Enabled;
extern RayTracingResolution g_RTShadows_Type;

// Environment
extern EnvironmentFile g_EnvironmentFile_Type;

extern bool g_ShadersCompilationSuccess;

// Path Tracing
extern u32 g_PathTrace_SppCached;
extern u32 g_PathTrace_SppNotCached;
extern u32 g_PathTrace_BounceCount;
extern bool g_RadianceCache_Trilinear;
extern u32 g_RadianceCache_TrilinearMinCornerSamples;
extern u32 g_RadianceCache_TrilinearMinHits;
extern u32 g_RadianceCache_TrilinearPresentMinSamples;
extern u32 g_RadianceCache_NormalBinRes;
extern u32 g_RadianceCache_MinExtraSppCount;
extern u32 g_RadianceCache_MaxAge;
extern u32 g_RadianceCache_MaxProbes;
extern u32 g_RadianceCache_MaxSamples;
extern f32 g_RadianceCache_CellSize;

struct ImGui_InitParams
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    const char* fontPath = "C:/Windows/Fonts/segoeui.ttf";
    f32 fontSize = 22.0f;
};

struct ImGui_TimingRaw
{
    const char* name;
    double ms;
};
struct ImGui_TimingSmooth
{
    const char* name;
    double value;
};

struct ImGui_FrameStats
{
    u32 fps;
    f32 cameraPos[3];
};

struct ImGui_RenderParams
{
    ID3D12GraphicsCommandList* cmd;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12Resource* rtvResource;

    ImGui_FrameStats frame;

    const ImGui_TimingRaw* timingsRaw = nullptr;
    u32 timingsRawCount = 0;
    const ImGui_TimingSmooth* timingsSmooth = nullptr;
    u32 timingsSmoothCount = 0;

    // Optional: GBuffer debug preview
    ID3D12Resource* gbufferAlbedo = nullptr;
    ID3D12Resource* gbufferNormal = nullptr;
    ID3D12Resource* gbufferNormalGeo = nullptr;
    ID3D12Resource* gbufferMaterial = nullptr;
    ID3D12Resource* gbufferMotion = nullptr;
    ID3D12Resource* gbufferAO = nullptr;
    ID3D12Resource* depth = nullptr;             // R32_TYPELESS depth buffer (viewed as R32_FLOAT)
    ID3D12Resource* rtShadows = nullptr;         // R16_FLOAT ray-traced shadows output
    ID3D12Resource* rtIndirectDiffuse = nullptr; // R16_FLOAT indirect diffuse output
    u32 renderWidth = 0;                         // for aspect ratio of previews
    u32 renderHeight = 0;                        // for aspect ratio of previews
};

void ImGui_Init(const ImGui_InitParams& p);
void ImGui_Shutdown();
void ImGui_Render(const ImGui_RenderParams& p);