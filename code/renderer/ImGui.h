// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>

#include "RuntimeState.h"

class Renderer;

struct ImGui_InitParams
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    HWND hwnd = nullptr;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    const char* fontPath = "C:/Windows/Fonts/segoeui.ttf";
    f32 fontSize = 22.0f;
};

struct ImGui_TimingValue
{
    const char* name;
    double ms;
};

struct ImGui_FrameStats
{
    u32 fps;
    f32 cameraPos[3];
    f32 cameraYaw;
    f32 cameraPitch;
};

struct ImGui_RenderParams
{
    ID3D12GraphicsCommandList* cmd;
    Renderer* renderer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    const char* loadingLabel = nullptr;

    ImGui_FrameStats frame;

    const ImGui_TimingValue* gpuTimings = nullptr;
    u32 gpuTimingsCount = 0;
    const ImGui_TimingValue* cpuTimings = nullptr;
    u32 cpuTimingsCount = 0;

    // Optional: GBuffer debug preview
    ID3D12Resource* gbufferAlbedo = nullptr;
    ID3D12Resource* gbufferNormal = nullptr;
    ID3D12Resource* gbufferNormalGeo = nullptr;
    ID3D12Resource* gbufferMaterial = nullptr;
    ID3D12Resource* gbufferMotion = nullptr;
    ID3D12Resource* gbufferAO = nullptr;
    ID3D12Resource* gbufferEmissive = nullptr;
    ID3D12Resource* depth = nullptr;             // R32_TYPELESS depth buffer (viewed as R32_FLOAT)
    ID3D12Resource* rtShadows = nullptr;         // R16_FLOAT ray-traced shadows output
    ID3D12Resource* rtIndirectDiffuse = nullptr; // R16_FLOAT indirect diffuse output
    ID3D12Resource* rtSpecular = nullptr;        // R16G16B16A16_FLOAT indirect specular output
    f32 radianceCacheUsedMB = 0.0f;              // Estimated occupied cache memory in MB
    f32 radianceCacheMaxMB = 0.0f;               // Total cache capacity in MB
};

void ImGui_Init(const ImGui_InitParams& p);
void ImGui_Shutdown();
void ImGui_ResetDebugTextureCache();
void ImGui_Render(const ImGui_RenderParams& p);

