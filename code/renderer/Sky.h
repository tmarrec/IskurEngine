// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include "BindlessHeaps.h"
#include "Camera.h"
#include "Constants.h"
#include "Environments.h"
#include "GBuffer.h"
#include "Shader.h"
#include "Texture.h"
#include "Timings.h"
#include "common/Types.h"
#include "shaders/CPUGPU.h"

class Raytracing;

class Sky
{
  public:
    void Terminate();

    void CreateProceduralSkyCubeResources(const ComPtr<ID3D12Device14>& device, BindlessHeaps& bindlessHeaps);
    void CreateSkyMotionPassResources(const ComPtr<ID3D12Device14>& device);

    void CreateProceduralSkyCubePipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines);
    void CreateSkyMotionPassPipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines);

    void PassSkyMotion(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, u32 frameInFlightIdx, const Camera::FrameData& cameraFrameData,
                       const Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>& gbufferRtvHandles, GpuResource& gbufferMotionVector, u32 depthSrvIndex,
                       const BindlessHeaps& bindlessHeaps);
    void PassProceduralSkyCube(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const Environment& env, const BindlessHeaps& bindlessHeaps,
                               Raytracing& raytracing);

    void MarkProceduralSkyDirty();
    u32 GetSkyCubeSrvIndex() const;

  private:
    struct ProceduralSkyCubeResources
    {
        static constexpr u32 skyCubeSize = 1024;

        Texture skyCube;

        SharedPtr<Shader> genSkyShader;
        ComPtr<ID3D12RootSignature> genSkyRootSig;
        ComPtr<ID3D12PipelineState> genSkyPso;

        bool dirty = true;
        XMFLOAT3 lastSunDir = {0.f, 0.f, 0.f};
        f32 lastSunIntensity = -1.0f;
        XMFLOAT3 lastSunColor = {-1.0f, -1.0f, -1.0f};
        f32 lastSunDiskAngleDeg = -1.0f;
        f32 lastSunGlowPower = -1.0f;
        f32 lastSunGlowIntensity = -1.0f;
        f32 lastSunDiskIntensityScale = -1.0f;
        f32 lastAtmosphereThicknessKm = -1.0f;
        f32 lastAtmosphereSunIntensityScale = -1.0f;
        f32 lastMieG = -1.0f;
        XMFLOAT3 lastRayleighScattering = {-1.0f, -1.0f, -1.0f};
        f32 lastRayleighScaleHeightKm = -1.0f;
        XMFLOAT3 lastMieScattering = {-1.0f, -1.0f, -1.0f};
        f32 lastMieScaleHeightKm = -1.0f;
        XMFLOAT3 lastOzoneAbsorption = {-1.0f, -1.0f, -1.0f};
        f32 lastOzoneLayerCenterKm = -1.0f;
        f32 lastOzoneLayerWidthKm = -1.0f;
        f32 lastMultiScatteringStrength = -1.0f;
    };

    struct SkyMotionPassResources
    {
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;
        SharedPtr<Shader> vxShader;
        SharedPtr<Shader> pxShader;

        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> cb;
        Array<u8*, IE_Constants::frameInFlightCount> cbMapped = {};
    };

    bool IsProceduralSkyDirty(const Environment& env) const;
    void FillSkyCubeGenConstants(SkyCubeGenConstants& outConstants, const Environment& env) const;
    void CacheProceduralSkyState(const Environment& env);
    void ReleaseSkyMotionPassResources();

    ProceduralSkyCubeResources m_ProceduralSkyCube{};
    SkyMotionPassResources m_SkyMotion{};
};

