// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "AutoExposure.h"

#include "PipelineHelpers.h"
#include "RenderDevice.h"
#include "RuntimeState.h"
#include "shaders/CPUGPU.h"

namespace
{
void ReleaseTextureDescriptors(BindlessHeaps& bindlessHeaps, Texture& texture)
{
    bindlessHeaps.FreeCbvSrvUav(texture.srvIndex);
    bindlessHeaps.FreeCbvSrvUav(texture.uavIndex);
    texture.srvIndex = UINT32_MAX;
    texture.uavIndex = UINT32_MAX;
}

void CreateExposureTexture(const ComPtr<ID3D12Device14>& device, BindlessHeaps& bindlessHeaps, Texture& outTexture, const wchar_t* name)
{
    ReleaseTextureDescriptors(bindlessHeaps, outTexture);

    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&outTexture.resource)));
    outTexture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outTexture.SetName(name);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    outTexture.srvIndex = bindlessHeaps.CreateSRV(outTexture.resource, srvDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = desc.Format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    outTexture.uavIndex = bindlessHeaps.CreateUAV(outTexture.resource, uavDesc);
}
} // namespace

void AutoExposure::CreateResources(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps)
{
    m_ResetHistory = true;
    const ComPtr<ID3D12Device14>& device = renderDevice.GetDevice();

    if (m_Resources.histogramBuffer)
    {
        bindlessHeaps.FreeCbvSrvUav(m_Resources.histogramBuffer->srvIndex);
        bindlessHeaps.FreeCbvSrvUav(m_Resources.histogramBuffer->uavIndex);
        m_Resources.histogramBuffer->srvIndex = UINT32_MAX;
        m_Resources.histogramBuffer->uavIndex = UINT32_MAX;
    }

    BufferCreateDesc bufferDesc;
    bufferDesc.heapType = D3D12_HEAP_TYPE_DEFAULT;
    bufferDesc.viewKind = BufferCreateDesc::ViewKind::Structured;
    bufferDesc.createSRV = true;
    bufferDesc.createUAV = true;
    bufferDesc.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bufferDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bufferDesc.finalState = bufferDesc.initialState;

    bufferDesc.sizeInBytes = m_Resources.numBuckets * sizeof(u32);
    bufferDesc.strideInBytes = sizeof(u32);
    bufferDesc.name = L"Histogram";
    m_Resources.histogramBuffer = renderDevice.CreateBuffer(bindlessHeaps, nullptr, bufferDesc);

    CreateExposureTexture(device, bindlessHeaps, m_Resources.exposureTexture, L"Exposure Inverse");
    CreateExposureTexture(device, bindlessHeaps, m_Resources.adaptedExposureTexture, L"Adapted Exposure Inverse");
    CreateExposureTexture(device, bindlessHeaps, m_Resources.finalExposureTexture, L"Final Exposure Scale");
}

void AutoExposure::InvalidateDescriptorIndices()
{
    if (m_Resources.histogramBuffer)
    {
        m_Resources.histogramBuffer->srvIndex = UINT32_MAX;
        m_Resources.histogramBuffer->uavIndex = UINT32_MAX;
    }

    m_Resources.exposureTexture.srvIndex = UINT32_MAX;
    m_Resources.exposureTexture.uavIndex = UINT32_MAX;
    m_Resources.adaptedExposureTexture.srvIndex = UINT32_MAX;
    m_Resources.adaptedExposureTexture.uavIndex = UINT32_MAX;
    m_Resources.finalExposureTexture.srvIndex = UINT32_MAX;
    m_Resources.finalExposureTexture.uavIndex = UINT32_MAX;
}

void AutoExposure::CreatePipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines)
{
    Shader::ReloadOrCreate(m_Resources.clearUintShader, IE_SHADER_TYPE_COMPUTE, "systems/exposure/clear_uint.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Resources.clearUintShader, m_Resources.clearUintRootSig, m_Resources.clearUintPso);

    Shader::ReloadOrCreate(m_Resources.histogramShader, IE_SHADER_TYPE_COMPUTE, "systems/exposure/histogram.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Resources.histogramShader, m_Resources.histogramRootSig, m_Resources.histogramPso);

    Shader::ReloadOrCreate(m_Resources.exposureShader, IE_SHADER_TYPE_COMPUTE, "systems/exposure/exposure.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Resources.exposureShader, m_Resources.exposureRootSig, m_Resources.exposurePso);

    Shader::ReloadOrCreate(m_Resources.adaptExposureShader, IE_SHADER_TYPE_COMPUTE, "systems/exposure/adapt_exposure.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Resources.adaptExposureShader, m_Resources.adaptExposureRootSig, m_Resources.adaptExposurePso);
}

void AutoExposure::Pass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const BindlessHeaps& bindlessHeaps, GpuResource& hdrTexture, u32 hdrSrvIndex, u32 depthSrvIndex,
                        const XMUINT2& renderSize, f32 frameTimeMs)
{
    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Auto Exposure");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        if (hdrTexture.state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            hdrTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        m_Resources.histogramBuffer->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_Resources.exposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_Resources.adaptedExposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_Resources.finalExposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ClearConstants clr;
        clr.bufferIndex = m_Resources.histogramBuffer->uavIndex;
        clr.numElements = m_Resources.histogramBuffer->numElements;
        cmd->SetPipelineState(m_Resources.clearUintPso.Get());
        cmd->SetComputeRootSignature(m_Resources.clearUintRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(clr) / 4, &clr, 0);
        cmd->Dispatch(IE_DivRoundUp(m_Resources.histogramBuffer->numElements, 64), 1, 1);

        m_Resources.histogramBuffer->UavBarrier(cmd);

        HistogramConstants hc;
        hc.hdrTextureIndex = hdrSrvIndex;
        hc.minLogLum = g_Settings.autoExposureMinLogLum;
        hc.maxLogLum = g_Settings.autoExposureMaxLogLum;
        hc.numBuckets = m_Resources.numBuckets;
        hc.histogramBufferIndex = m_Resources.histogramBuffer->uavIndex;
        hc.depthTextureIndex = depthSrvIndex;
        cmd->SetPipelineState(m_Resources.histogramPso.Get());
        cmd->SetComputeRootSignature(m_Resources.histogramRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(hc) / 4, &hc, 0);
        cmd->Dispatch(IE_DivRoundUp(renderSize.x, 16), IE_DivRoundUp(renderSize.y, 16), 1);

        m_Resources.histogramBuffer->UavBarrier(cmd);
        m_Resources.histogramBuffer->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        ExposureConstants ec;
        ec.numBuckets = m_Resources.numBuckets;
        ec.totalPixels = 0; // exposure pass computes weighted count from histogram
        ec.targetPct = g_Settings.autoExposureTargetPct;
        ec.lowReject = g_Settings.autoExposureLowReject;
        ec.highReject = g_Settings.autoExposureHighReject;
        ec.key = g_Settings.autoExposureKey;
        ec.minLogLum = g_Settings.autoExposureMinLogLum;
        ec.maxLogLum = g_Settings.autoExposureMaxLogLum;
        ec.histogramBufferIndex = m_Resources.histogramBuffer->srvIndex;
        ec.exposureBufferIndex = m_Resources.exposureTexture.uavIndex;
        cmd->SetPipelineState(m_Resources.exposurePso.Get());
        cmd->SetComputeRootSignature(m_Resources.exposureRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(ec) / 4, &ec, 0);
        cmd->Dispatch(1, 1, 1);

        m_Resources.exposureTexture.UavBarrier(cmd);
        m_Resources.exposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        AdaptExposureConstants ac;
        ac.exposureBufferIndex = m_Resources.exposureTexture.srvIndex;
        ac.adaptedExposureBufferIndex = m_Resources.adaptedExposureTexture.uavIndex;
        ac.finalExposureTextureIndex = m_Resources.finalExposureTexture.uavIndex;
        ac.resetHistory = m_ResetHistory ? 1u : 0u;
        ac.dt = frameTimeMs / 1000.0f;
        ac.tauBright = g_Settings.autoExposureTauBright;
        ac.tauDark = g_Settings.autoExposureTauDark;
        ac.clampMin = g_Settings.autoExposureClampMin;
        ac.clampMax = g_Settings.autoExposureClampMax;
        ac.exposureCompensationEV = g_Settings.toneMappingExposureCompensationEV;
        cmd->SetPipelineState(m_Resources.adaptExposurePso.Get());
        cmd->SetComputeRootSignature(m_Resources.adaptExposureRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(ac) / 4, &ac, 0);
        cmd->Dispatch(1, 1, 1);

        m_Resources.adaptedExposureTexture.UavBarrier(cmd);
        m_Resources.adaptedExposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        m_Resources.finalExposureTexture.UavBarrier(cmd);
        m_Resources.finalExposureTexture.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        m_ResetHistory = false;
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

u32 AutoExposure::GetFinalExposureTextureSrvIndex() const
{
    return m_Resources.finalExposureTexture.srvIndex;
}

const Texture& AutoExposure::GetFinalExposureTexture() const
{
    return m_Resources.finalExposureTexture;
}
