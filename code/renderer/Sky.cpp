// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Sky.h"

#include "PipelineHelpers.h"
#include "Raytracing.h"
#include "RuntimeState.h"

namespace
{
constexpr u32 kSkyCubeGenRootConstantsCount = sizeof(SkyCubeGenConstants) / sizeof(u32);
constexpr u32 kGBufferMotionTargetIndex = 4;
constexpr f32 kSunDiskSoftness = 1.4f;
static_assert(kGBufferMotionTargetIndex < GBuffer::targetCount);

bool HasChanged3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return IE_Fabs(a.x - b.x) > 1e-4f || IE_Fabs(a.y - b.y) > 1e-4f || IE_Fabs(a.z - b.z) > 1e-4f;
}

bool HasChanged1(f32 a, f32 b)
{
    return IE_Fabs(a - b) > 1e-4f;
}
} // namespace

void Sky::Terminate()
{
    ReleaseSkyMotionPassResources();
}

void Sky::PassSkyMotion(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, u32 frameInFlightIdx, const Camera::FrameData& cameraFrameData,
                        const Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>& gbufferRtvHandles, GpuResource& gbufferMotionVector, u32 depthSrvIndex, const BindlessHeaps& bindlessHeaps)
{
    IE_Assert(frameInFlightIdx < IE_Constants::frameInFlightCount);
    IE_Assert(m_SkyMotion.cbMapped[frameInFlightIdx] != nullptr);

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Sky Motion Vectors");
    {
        gbufferMotionVector.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

        SkyMotionPassConstants c{};
        c.depthTextureIndex = depthSrvIndex;
        c.invView = cameraFrameData.invView;
        c.prevView = cameraFrameData.prevView;
        c.projectionNoJitter = cameraFrameData.projectionNoJitter;
        c.prevProjectionNoJitter = cameraFrameData.prevProjectionNoJitter;
        std::memcpy(m_SkyMotion.cbMapped[frameInFlightIdx], &c, sizeof(c));

        cmd->OMSetRenderTargets(1, &gbufferRtvHandles[kGBufferMotionTargetIndex], false, nullptr);
        cmd->SetPipelineState(m_SkyMotion.pso.Get());
        cmd->SetDescriptorHeaps(bindlessHeaps.GetDescriptorHeaps().size(), bindlessHeaps.GetDescriptorHeaps().data());
        cmd->SetGraphicsRootSignature(m_SkyMotion.rootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_SkyMotion.cb[frameInFlightIdx]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);

        gbufferMotionVector.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

bool Sky::IsProceduralSkyDirty(const Environment& env) const
{
    const Environment::SkySettings& sky = env.sky;
    const Environment::SkySettings::AtmosphereSettings& atmosphere = sky.atmosphere;

    return HasChanged3(env.sunDir, m_ProceduralSkyCube.lastSunDir) || HasChanged1(g_Settings.sunIntensity, m_ProceduralSkyCube.lastSunIntensity) ||
           HasChanged3(sky.sunColor, m_ProceduralSkyCube.lastSunColor) || HasChanged1(sky.sunDiskAngleDeg, m_ProceduralSkyCube.lastSunDiskAngleDeg) ||
           HasChanged1(sky.sunGlowPower, m_ProceduralSkyCube.lastSunGlowPower) || HasChanged1(sky.sunGlowIntensity, m_ProceduralSkyCube.lastSunGlowIntensity) ||
           HasChanged1(sky.sunDiskIntensityScale, m_ProceduralSkyCube.lastSunDiskIntensityScale) ||
           HasChanged1(atmosphere.atmosphereThicknessKm, m_ProceduralSkyCube.lastAtmosphereThicknessKm) ||
           HasChanged1(atmosphere.sunIntensityScale, m_ProceduralSkyCube.lastAtmosphereSunIntensityScale) || HasChanged1(atmosphere.mieG, m_ProceduralSkyCube.lastMieG) ||
           HasChanged3(atmosphere.rayleighScattering, m_ProceduralSkyCube.lastRayleighScattering) || HasChanged1(atmosphere.rayleighScaleHeightKm, m_ProceduralSkyCube.lastRayleighScaleHeightKm) ||
           HasChanged3(atmosphere.mieScattering, m_ProceduralSkyCube.lastMieScattering) || HasChanged1(atmosphere.mieScaleHeightKm, m_ProceduralSkyCube.lastMieScaleHeightKm) ||
           HasChanged3(atmosphere.ozoneAbsorption, m_ProceduralSkyCube.lastOzoneAbsorption) || HasChanged1(atmosphere.ozoneLayerCenterKm, m_ProceduralSkyCube.lastOzoneLayerCenterKm) ||
           HasChanged1(atmosphere.ozoneLayerWidthKm, m_ProceduralSkyCube.lastOzoneLayerWidthKm) || HasChanged1(atmosphere.multiScatteringStrength, m_ProceduralSkyCube.lastMultiScatteringStrength);
}

void Sky::FillSkyCubeGenConstants(SkyCubeGenConstants& outConstants, const Environment& env) const
{
    const Environment::SkySettings& sky = env.sky;
    const Environment::SkySettings::AtmosphereSettings& atmosphere = sky.atmosphere;

    outConstants.outUavIndex = m_ProceduralSkyCube.skyCube.uavIndex;
    outConstants.size = ProceduralSkyCubeResources::skyCubeSize;
    outConstants.sunDir = env.sunDir;
    outConstants.sunIntensity = g_Settings.sunIntensity;
    outConstants.sunColor = sky.sunColor;
    outConstants.sunDiskAngleDeg = sky.sunDiskAngleDeg;
    outConstants.sunDiskSoftness = kSunDiskSoftness;
    outConstants.sunGlowPower = sky.sunGlowPower;
    outConstants.sunGlowIntensity = sky.sunGlowIntensity;
    outConstants.sunDiskIntensityScale = sky.sunDiskIntensityScale;
    outConstants.atmosphereThicknessKm = atmosphere.atmosphereThicknessKm;
    outConstants.atmosphereSunIntensityScale = atmosphere.sunIntensityScale;
    outConstants.mieG = atmosphere.mieG;
    outConstants.rayleighScattering = atmosphere.rayleighScattering;
    outConstants.rayleighScaleHeightKm = atmosphere.rayleighScaleHeightKm;
    outConstants.mieScattering = atmosphere.mieScattering;
    outConstants.mieScaleHeightKm = atmosphere.mieScaleHeightKm;
    outConstants.ozoneAbsorption = atmosphere.ozoneAbsorption;
    outConstants.ozoneLayerCenterKm = atmosphere.ozoneLayerCenterKm;
    outConstants.ozoneLayerWidthKm = atmosphere.ozoneLayerWidthKm;
    outConstants.multiScatteringStrength = atmosphere.multiScatteringStrength;
}

void Sky::CacheProceduralSkyState(const Environment& env)
{
    const Environment::SkySettings& sky = env.sky;
    const Environment::SkySettings::AtmosphereSettings& atmosphere = sky.atmosphere;

    m_ProceduralSkyCube.lastSunDir = env.sunDir;
    m_ProceduralSkyCube.lastSunIntensity = g_Settings.sunIntensity;
    m_ProceduralSkyCube.lastSunColor = sky.sunColor;
    m_ProceduralSkyCube.lastSunDiskAngleDeg = sky.sunDiskAngleDeg;
    m_ProceduralSkyCube.lastSunGlowPower = sky.sunGlowPower;
    m_ProceduralSkyCube.lastSunGlowIntensity = sky.sunGlowIntensity;
    m_ProceduralSkyCube.lastSunDiskIntensityScale = sky.sunDiskIntensityScale;
    m_ProceduralSkyCube.lastAtmosphereThicknessKm = atmosphere.atmosphereThicknessKm;
    m_ProceduralSkyCube.lastAtmosphereSunIntensityScale = atmosphere.sunIntensityScale;
    m_ProceduralSkyCube.lastMieG = atmosphere.mieG;
    m_ProceduralSkyCube.lastRayleighScattering = atmosphere.rayleighScattering;
    m_ProceduralSkyCube.lastRayleighScaleHeightKm = atmosphere.rayleighScaleHeightKm;
    m_ProceduralSkyCube.lastMieScattering = atmosphere.mieScattering;
    m_ProceduralSkyCube.lastMieScaleHeightKm = atmosphere.mieScaleHeightKm;
    m_ProceduralSkyCube.lastOzoneAbsorption = atmosphere.ozoneAbsorption;
    m_ProceduralSkyCube.lastOzoneLayerCenterKm = atmosphere.ozoneLayerCenterKm;
    m_ProceduralSkyCube.lastOzoneLayerWidthKm = atmosphere.ozoneLayerWidthKm;
    m_ProceduralSkyCube.lastMultiScatteringStrength = atmosphere.multiScatteringStrength;
}

void Sky::PassProceduralSkyCube(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const Environment& env, const BindlessHeaps& bindlessHeaps)
{
    static_assert(kSkyCubeGenRootConstantsCount == 36);
    if (IsProceduralSkyDirty(env))
    {
        m_ProceduralSkyCube.dirty = true;
    }

    if (!m_ProceduralSkyCube.dirty)
    {
        return;
    }

    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Sky Cube Update");
    {
        m_ProceduralSkyCube.skyCube.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_ProceduralSkyCube.genSkyRootSig.Get());
        cmd->SetPipelineState(m_ProceduralSkyCube.genSkyPso.Get());

        SkyCubeGenConstants c{};
        FillSkyCubeGenConstants(c, env);
        cmd->SetComputeRoot32BitConstants(0, kSkyCubeGenRootConstantsCount, &c, 0);
        cmd->Dispatch(IE_DivRoundUp(ProceduralSkyCubeResources::skyCubeSize, 8), IE_DivRoundUp(ProceduralSkyCubeResources::skyCubeSize, 8), 6);

        m_ProceduralSkyCube.skyCube.UavBarrier(cmd);
        m_ProceduralSkyCube.skyCube.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    CacheProceduralSkyState(env);
    m_ProceduralSkyCube.dirty = false;
}

void Sky::CreateProceduralSkyCubePipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines)
{
    Shader::ReloadOrCreate(m_ProceduralSkyCube.genSkyShader, IE_SHADER_TYPE_COMPUTE, "systems/sky/sky_cube_gen.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_ProceduralSkyCube.genSkyShader, m_ProceduralSkyCube.genSkyRootSig, m_ProceduralSkyCube.genSkyPso);
}

void Sky::CreateSkyMotionPassPipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines)
{
    Shader::ReloadOrCreate(m_SkyMotion.vxShader, IE_SHADER_TYPE_VERTEX, "shared/fullscreen.vs.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_SkyMotion.pxShader, IE_SHADER_TYPE_PIXEL, "systems/sky/sky_motion.ps.hlsl", globalDefines);
    PipelineHelpers::CreateFullscreenGraphicsPipeline(device, m_SkyMotion.vxShader, m_SkyMotion.pxShader, DXGI_FORMAT_R16G16_FLOAT, m_SkyMotion.rootSig, m_SkyMotion.pso);
}

void Sky::CreateProceduralSkyCubeResources(const ComPtr<ID3D12Device14>& device, BindlessHeaps& bindlessHeaps)
{
    m_ProceduralSkyCube.skyCube.Reset();
    m_ProceduralSkyCube.skyCube.srvIndex = UINT32_MAX;
    m_ProceduralSkyCube.skyCube.uavIndex = UINT32_MAX;

    auto CreateCube = [&](u32 size, u32 mipCount, const wchar_t* name, Texture& out) {
        D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, size, size, 6, mipCount, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        IE_Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out.resource)));
        out.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        out.SetName(name);
    };

    CreateCube(ProceduralSkyCubeResources::skyCubeSize, 1, L"Procedural Sky Cube", m_ProceduralSkyCube.skyCube);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MostDetailedMip = 0;
    srv.TextureCube.ResourceMinLODClamp = 0.0f;

    srv.TextureCube.MipLevels = 1;
    m_ProceduralSkyCube.skyCube.srvIndex = bindlessHeaps.CreateSRV(m_ProceduralSkyCube.skyCube.resource, srv);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.FirstArraySlice = 0;
    uav.Texture2DArray.ArraySize = 6;
    uav.Texture2DArray.PlaneSlice = 0;

    uav.Texture2DArray.MipSlice = 0;
    m_ProceduralSkyCube.skyCube.uavIndex = bindlessHeaps.CreateUAV(m_ProceduralSkyCube.skyCube.resource, uav);

    m_ProceduralSkyCube.dirty = true;
    m_ProceduralSkyCube.lastSunIntensity = -1.0f;
    m_ProceduralSkyCube.lastSunDiskAngleDeg = -1.0f;
    m_ProceduralSkyCube.lastSunGlowPower = -1.0f;
    m_ProceduralSkyCube.lastSunGlowIntensity = -1.0f;
    m_ProceduralSkyCube.lastSunDiskIntensityScale = -1.0f;
    m_ProceduralSkyCube.lastAtmosphereThicknessKm = -1.0f;
    m_ProceduralSkyCube.lastAtmosphereSunIntensityScale = -1.0f;
    m_ProceduralSkyCube.lastMieG = -1.0f;
    m_ProceduralSkyCube.lastRayleighScattering = {-1.0f, -1.0f, -1.0f};
    m_ProceduralSkyCube.lastRayleighScaleHeightKm = -1.0f;
    m_ProceduralSkyCube.lastMieScattering = {-1.0f, -1.0f, -1.0f};
    m_ProceduralSkyCube.lastMieScaleHeightKm = -1.0f;
    m_ProceduralSkyCube.lastOzoneAbsorption = {-1.0f, -1.0f, -1.0f};
    m_ProceduralSkyCube.lastOzoneLayerCenterKm = -1.0f;
    m_ProceduralSkyCube.lastOzoneLayerWidthKm = -1.0f;
    m_ProceduralSkyCube.lastMultiScatteringStrength = -1.0f;
}

void Sky::CreateSkyMotionPassResources(const ComPtr<ID3D12Device14>& device)
{
    ReleaseSkyMotionPassResources();

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(IE_AlignUp(sizeof(SkyMotionPassConstants), 256));
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_SkyMotion.cb[i])));
        IE_Check(m_SkyMotion.cb[i]->SetName(L"SkyMotionPassConstants"));
        IE_Check(m_SkyMotion.cb[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_SkyMotion.cbMapped[i])));
    }
}

void Sky::ReleaseSkyMotionPassResources()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        if (m_SkyMotion.cb[i] && m_SkyMotion.cbMapped[i])
        {
            m_SkyMotion.cb[i]->Unmap(0, nullptr);
        }
        m_SkyMotion.cbMapped[i] = nullptr;
        m_SkyMotion.cb[i].Reset();
    }
}

void Sky::MarkProceduralSkyDirty()
{
    m_ProceduralSkyCube.dirty = true;
}

u32 Sky::GetSkyCubeSrvIndex() const
{
    return m_ProceduralSkyCube.skyCube.srvIndex;
}
