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
extern f32 g_IBL_DiffuseIntensity;
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
extern f32 g_RTShadows_IBLDiffuseIntensity;
extern f32 g_RTShadows_IBLSpecularIntensity;

// SSAO
extern f32 g_SSAO_SampleRadius;
extern f32 g_SSAO_SampleBias;
extern f32 g_SSAO_Power;

struct ImGui_InitParams
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    const char* fontPath = "C:/Windows/Fonts/segoeui.ttf";
    f32 fontSize = 21.0f;
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
    ID3D12Resource* gbufferMaterial = nullptr;
    ID3D12Resource* gbufferMotion = nullptr;
    ID3D12Resource* gbufferAO = nullptr;
    ID3D12Resource* depth = nullptr;     // R32_TYPELESS depth buffer (viewed as R32_FLOAT)
    ID3D12Resource* rtShadows = nullptr; // R16_FLOAT ray-traced shadows output
    ID3D12Resource* ssao = nullptr;      // R8_UNORM SSAO texture (optional)
    u32 renderWidth = 0;                 // for aspect ratio of previews
    u32 renderHeight = 0;                // for aspect ratio of previews
};

void ImGui_Init(const ImGui_InitParams& p);
void ImGui_Shutdown();
void ImGui_Render(const ImGui_RenderParams& p);