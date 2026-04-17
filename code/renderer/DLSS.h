// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

namespace DLSS
{
enum class Mode : u8
{
    DLAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

struct OptimalSettings
{
    u32 renderWidth = 0;
    u32 renderHeight = 0;
    u32 renderWidthMin = 0;
    u32 renderHeightMin = 0;
    u32 renderWidthMax = 0;
    u32 renderHeightMax = 0;
    f32 sharpness = 0.0f;
};

struct CommonConstantsDesc
{
    u32 streamlineFrameIndex = 0;

    XMFLOAT4X4 projectionNoJitter;
    XMFLOAT4X4 prevProjectionNoJitter;
    XMFLOAT4X4 invView;
    XMFLOAT4X4 prevView;
    XMFLOAT3 cameraPos;

    f32 cameraFov = 0.0f;
    f32 cameraAspect = 0.0f;
    f32 cameraNear = 0.0f;
    f32 cameraFar = 0.0f;
    f32 jitterOffsetX = 0.0f; // pixel space
    f32 jitterOffsetY = 0.0f; // pixel space
    bool reset = false;
};

struct EvaluateDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;

    ID3D12Resource* colorIn = nullptr;
    ID3D12Resource* colorOut = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* diffuseAlbedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* normalRoughness = nullptr;
    ID3D12Resource* specularHitDistance = nullptr;

    DXGI_FORMAT colorInFormat;
    DXGI_FORMAT colorOutFormat;
    DXGI_FORMAT depthFormat;
    DXGI_FORMAT motionVectorsFormat;
    DXGI_FORMAT exposureFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT diffuseAlbedoFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT specularAlbedoFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT normalRoughnessFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT specularHitDistanceFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_RESOURCE_STATES colorInState;
    D3D12_RESOURCE_STATES colorOutState;
    D3D12_RESOURCE_STATES depthState;
    D3D12_RESOURCE_STATES motionVectorsState;
    D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES diffuseAlbedoState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES specularAlbedoState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES normalRoughnessState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES specularHitDistanceState = D3D12_RESOURCE_STATE_COMMON;

    u32 renderWidth = 0;
    u32 renderHeight = 0;
    u32 outputWidth = 0;
    u32 outputHeight = 0;
    u32 streamlineFrameIndex = 0;
    Mode mode = Mode::Quality;
    XMFLOAT4X4 invView;
};

struct FrameGenerationOptionsDesc
{
    bool enabled = false;
    u32 renderWidth = 0;
    u32 renderHeight = 0;
    u32 outputWidth = 0;
    u32 outputHeight = 0;
    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motionVectorsFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uiFormat = DXGI_FORMAT_UNKNOWN;
};

struct FrameGenerationTagDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    u32 streamlineFrameIndex = 0;
    bool valid = false;

    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* hudlessColor = nullptr;
    ID3D12Resource* uiColorAndAlpha = nullptr;

    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motionVectorsFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uiFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES motionVectorsState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;

    u32 renderWidth = 0;
    u32 renderHeight = 0;
    u32 outputWidth = 0;
    u32 outputHeight = 0;
};

struct FrameGenerationState
{
    u32 status = 0;
    u32 numFramesActuallyPresented = 0;
};

void GetOptimalSettings(u32 outputWidth, u32 outputHeight, Mode mode, OptimalSettings& outSettings);
void SetCommonConstants(const CommonConstantsDesc& desc);
void Evaluate(const EvaluateDesc& desc);
void SetFrameGenerationFeatureLoaded(bool loaded);
void SetFrameGenerationOptions(const FrameGenerationOptionsDesc& desc);
void TagFrameGenerationInputs(const FrameGenerationTagDesc& desc);
void GetFrameGenerationState(FrameGenerationState& outState);
bool ConsumeFrameGenerationApiError(i32& outError);
void VerifyLoaded();
} // namespace DLSS
