// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Renderer.h"

#include <DDSTextureLoader.h>
#include <ResourceUploadBatch.h>
#include <directx/d3dx12.h>
#include <dx12/ffx_api_dx12.hpp>
#include <ffx_api.hpp>
#include <ffx_api_loader.h>
#include <ffx_upscale.hpp>

#include "Camera.h"
#include "Constants.h"
#include "ImGui.h"
#include "Raytracing.h"
#include "SceneLoader.h"
#include "common/Asserts.h"
#include "common/CommandLineArguments.h"
#include "common/Types.h"
#include "common/math/MathUtils.h"
#include "window/Window.h"

void Renderer::Init()
{
    CreateDevice();
    CreateAllocator();
    CreateCommandQueue();
    CreateCommands();
    CreateFrameSynchronizationFences();

    SetRenderAndPresentSize();

    m_BindlessHeaps.Init(m_Device);

    CreateSwapchain();
    CreateRTVs();
    CreateDSV();

    AllocateUploadBuffer(nullptr, sizeof(VertexConstants) * IE_Constants::frameInFlightCount, 0, m_ConstantsBuffer, m_ConstantsBufferAlloc, L"Color Constants");

    LoadScene();

    CreateGPUTimers();

    CreateFSRPassResources();

    Vector<WString> globalDefines;
    CreateDepthPrePassResources(globalDefines);
    CreateGBufferPassResources(globalDefines);
    CreateLightingPassResources(globalDefines);
    CreateHistogramPassResources(globalDefines);
    CreateToneMapPassResources(globalDefines);
    CreateSSAOResources(globalDefines);

    ImGui_InitParams imGuiInitParams;
    imGuiInitParams.device = m_Device.Get();
    imGuiInitParams.queue = m_CommandQueue.Get();
    imGuiInitParams.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ImGui_Init(imGuiInitParams);
}

void Renderer::Terminate()
{
    // Wait for last frame execution
    ComPtr<ID3D12Fence> fence;
    IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    IE_Check(m_CommandQueue->Signal(fence.Get(), 1));

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    IE_Check(fence->SetEventOnCompletion(1, evt));
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);

    // Raytracing
    Raytracing::GetInstance().Terminate();
    Raytracing::DestroyInstance();

    // FSR
    IE_Assert(ffx::DestroyContext(m_Fsr.context) == ffx::ReturnCode::Ok);

    // ImGui
    ImGui_Shutdown();
}

void Renderer::Render()
{
    m_FrameInFlightIdx = m_Swapchain->GetCurrentBackBufferIndex();
    PerFrameData& frameData = GetCurrentFrameData();
    ComPtr<ID3D12GraphicsCommandList7> cmd;
    Camera::FrameData cameraFrameData{};
    f32 jitterNormX = 0.f, jitterNormY = 0.f;

    BeginFrame(frameData, cmd, cameraFrameData, jitterNormX, jitterNormY);

    Pass_DepthPre(cmd);
    Pass_Raytracing(cmd);
    Pass_GBuffer(cmd);
    Pass_SSAO(cmd, cameraFrameData);
    Pass_Lighting(cmd, cameraFrameData);
    Pass_FSR(cmd, jitterNormX, jitterNormY, cameraFrameData);
    Pass_Histogram(cmd);
    Pass_Tonemap(cmd);
    Pass_ImGui(cmd, cameraFrameData);

    EndFrame(frameData, cmd);
}

void Renderer::BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY)
{
    WaitOnFence(frameData.frameFence, frameData.frameFenceValue);

    // Timings
    GpuTimings_Collect(frameData.gpuTimers, m_GpuTimingState);
    GpuTimings_UpdateAverages(m_GpuTimingState, Window::GetFrameTimeMs(), g_Timing_AverageWindowMs);
    frameData.gpuTimers.passCount = 0;
    frameData.gpuTimers.nextIdx = 0;

    // Jitter
    m_Fsr.jitterIndex = (m_Fsr.jitterIndex + 1) % m_Fsr.jitterPhaseCount;

    ffx::QueryDescUpscaleGetJitterOffset jo{};
    jo.index = m_Fsr.jitterIndex;
    jo.phaseCount = m_Fsr.jitterPhaseCount;
    jo.pOutX = &m_Fsr.jitterX;
    jo.pOutY = &m_Fsr.jitterY;
    IE_Assert(ffx::Query(m_Fsr.context, jo) == ffx::ReturnCode::Ok);

    jitterNormX = m_Fsr.jitterX * 2.0f / static_cast<f32>(m_Fsr.renderSize.x);
    jitterNormY = -m_Fsr.jitterY * 2.0f / static_cast<f32>(m_Fsr.renderSize.y);

    // Camera
    Camera& camera = Camera::GetInstance();
    camera.ConfigurePerspective(Window::GetInstance().GetAspectRatio(), IE_ToRadians(g_Camera_FOV), IE_ToRadians(g_Camera_FrustumCullingFOV), 0.1f, jitterNormX, jitterNormY);

    // Sun
    float cosE = cosf(g_Sun_Elevation);
    float sinE = sinf(g_Sun_Elevation);
    XMVECTOR sun = XMVectorSet(cosE * cosf(g_Sun_Azimuth), sinE, cosE * sinf(g_Sun_Azimuth), 0.0f);
    sun = XMVector3Normalize(sun);
    XMStoreFloat3(&m_SunDir, sun);

    cameraFrameData = camera.GetFrameData();

    // Constant Buffer
    VertexConstants constants;
    constants.cameraPos = cameraFrameData.position;
    constants.viewProj = cameraFrameData.viewProj;
    constants.view = cameraFrameData.view;
    constants.viewProjNoJ = cameraFrameData.viewProjNoJ;
    constants.prevViewProjNoJ = cameraFrameData.prevViewProjNoJ;
    std::memcpy(constants.planes, cameraFrameData.frustumCullingPlanes, sizeof(cameraFrameData.frustumCullingPlanes));
    SetResourceBufferData(m_ConstantsBuffer, &constants, sizeof(constants), m_FrameInFlightIdx * sizeof(constants));

    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));
    cmd = frameData.cmd;

    cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
    cmd->RSSetViewports(1, &m_RenderViewport);
    cmd->RSSetScissorRects(1, &m_RenderRect);
}

void Renderer::Pass_DepthPre(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    // Transition
    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Clear
    cmd->OMSetRenderTargets(0, nullptr, false, &m_DepthPre.dsvHandles[m_FrameInFlightIdx]);
    cmd->ClearDepthStencilView(m_DepthPre.dsvHandles[m_FrameInFlightIdx], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    auto drawPrimitives = [&](const Vector<PrimitiveRenderData>& primitivesRenderData) {
        for (const PrimitiveRenderData& primitiveRenderData : primitivesRenderData)
        {
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(primitiveRenderData.primConstants) / 4, &primitiveRenderData.primConstants, 0);
            cmd->DispatchMesh(IE_DivRoundUp(m_Primitives[primitiveRenderData.primIndex].meshletCount, 32), 1, 1);
        }
    };

    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Pre-Pass - Opaque");
    {
        cmd->SetGraphicsRootSignature(m_DepthPre.opaqueRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));
        for (CullMode cm : {CullMode_Back, CullMode_None})
        {
            cmd->SetPipelineState(m_DepthPre.opaquePSO[cm].Get());
            drawPrimitives(m_PrimitivesRenderData[AlphaMode_Opaque][cm]);
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Pre-Pass - Alpha-Tested");
    {
        cmd->SetGraphicsRootSignature(m_DepthPre.alphaTestRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));
        for (CullMode cm : {CullMode_Back, CullMode_None})
        {
            cmd->SetPipelineState(m_DepthPre.alphaTestPSO[cm].Get());
            drawPrimitives(m_PrimitivesRenderData[AlphaMode_Mask][cm]);
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
}

void Renderer::Pass_Raytracing(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    Raytracing& raytracing = Raytracing::GetInstance();
    if (g_RTShadows_Enabled)
    {
        Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_GENERIC_READ);

        Raytracing::ShadowPassInput shadowPassInput;
        shadowPassInput.depthSamplerIndex = m_LinearSamplerIdx;
        shadowPassInput.frameIndex = m_FrameIndex;
        shadowPassInput.sunDir = m_SunDir;
        shadowPassInput.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
        raytracing.ShadowPass(cmd, shadowPassInput);

        Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_READ);
    }
}

void Renderer::Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    auto& gb = m_GBuf.gbuffers[m_FrameInFlightIdx];

    auto BarrierGBuffer = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        Barrier(cmd, gb.albedo, from, to);
        Barrier(cmd, gb.normal, from, to);
        Barrier(cmd, gb.material, from, to);
        Barrier(cmd, gb.motionVector, from, to);
        Barrier(cmd, gb.ao, from, to);
    };

    BarrierGBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    f32 clearColor[4] = {0, 0, 0, 0};
    for (u8 i = 0; i < GBuffer::targetCount; ++i)
    {
        cmd->ClearRenderTargetView(m_GBuf.rtvHandles[m_FrameInFlightIdx][i], clearColor, 0, nullptr);
    }

    auto drawPrimitives = [&](const Vector<PrimitiveRenderData>& primitivesRenderData) {
        for (const PrimitiveRenderData& primitiveRenderData : primitivesRenderData)
        {
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(primitiveRenderData.primConstants) / 4, &primitiveRenderData.primConstants, 0);
            cmd->DispatchMesh(IE_DivRoundUp(m_Primitives[primitiveRenderData.primIndex].meshletCount, 32), 1, 1);
        }
    };

    for (AlphaMode alphaMode : {AlphaMode_Opaque, AlphaMode_Mask})
    {
        const char* passName = alphaMode == AlphaMode_Opaque ? "GBuffer Opaque Pass" : "GBuffer Masked Pass";
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, passName);
        {
            cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
            cmd->SetGraphicsRootSignature(m_GBuf.rootSigs[alphaMode].Get());
            cmd->OMSetRenderTargets(GBuffer::targetCount, m_GBuf.rtvHandles[m_FrameInFlightIdx].data(), false, &m_DepthPre.dsvHandles[m_FrameInFlightIdx]);
            cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));

            cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_Back].Get());
            drawPrimitives(m_PrimitivesRenderData[alphaMode][CullMode_Back]);

            cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_None].Get());
            drawPrimitives(m_PrimitivesRenderData[alphaMode][CullMode_None]);
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    BarrierGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Renderer::Pass_SSAO(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();
    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "SSAO Pass");
    {
        Barrier(cmd, m_Ssao.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_Ssao.pso.Get());
        cmd->SetComputeRootSignature(m_Ssao.rootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

        SSAOConstants ssaoConstants{};
        ssaoConstants.radius = g_SSAO_SampleRadius;
        ssaoConstants.bias = g_SSAO_SampleBias;
        ssaoConstants.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
        ssaoConstants.normalTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normalIndex;
        ssaoConstants.proj = cameraFrameData.projection;
        ssaoConstants.invProj = cameraFrameData.invProjJ;
        ssaoConstants.view = cameraFrameData.view;
        ssaoConstants.renderTargetSize = {static_cast<f32>(m_Fsr.renderSize.x), static_cast<f32>(m_Fsr.renderSize.y)};
        ssaoConstants.ssaoTextureIndex = m_Ssao.uavIdx;
        ssaoConstants.samplerIndex = m_LinearSamplerIdx;
        ssaoConstants.zNear = cameraFrameData.znearfar.x;
        ssaoConstants.power = g_SSAO_Power;
        cmd->SetComputeRoot32BitConstants(0, sizeof(SSAOConstants) / 4, &ssaoConstants, 0);

        cmd->Dispatch(IE_DivRoundUp(m_Fsr.renderSize.x, 16), IE_DivRoundUp(m_Fsr.renderSize.y, 16), 1);
        UAVBarrier(cmd, m_Ssao.texture);
        Barrier(cmd, m_Ssao.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].material, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].ao, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::Pass_Lighting(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();
    const Raytracing::ShadowPassOutput& shadowPassOutput = Raytracing::GetInstance().GetShadowPassOutput();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Lighting Pass");
    {
        LightingPassConstants c;
        c.albedoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].albedoIndex;
        c.normalTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normalIndex;
        c.materialTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].materialIndex;
        c.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
        c.samplerIndex = m_LinearSamplerIdx;
        c.cameraPos = cameraFrameData.position;
        c.view = cameraFrameData.view;
        c.invView = cameraFrameData.invView;
        c.invViewProj = cameraFrameData.invViewProj;
        c.sunDir = m_SunDir;
        c.raytracingOutputIndex = shadowPassOutput.srvIndex;
        c.envMapIndex = m_Env.envSrvIdx;
        c.diffuseIBLIndex = m_Env.diffuseSrvIdx;
        c.specularIBLIndex = m_Env.specularSrvIdx;
        c.brdfLUTIndex = m_Env.brdfSrvIdx;
        c.sunAzimuth = g_Sun_Azimuth;
        c.IBLDiffuseIntensity = g_IBL_DiffuseIntensity;
        c.IBLSpecularIntensity = g_IBL_SpecularIntensity;
        c.RTShadowsEnabled = g_RTShadows_Enabled;
        c.RTShadowsIBLDiffuseStrength = g_RTShadows_IBLDiffuseIntensity;
        c.RTShadowsIBLSpecularStrength = g_RTShadows_IBLSpecularIntensity;
        c.renderSize = {static_cast<f32>(m_Fsr.renderSize.x), static_cast<f32>(m_Fsr.renderSize.y)};
        c.ssaoTextureIndex = m_Ssao.srvIdx;
        c.sunIntensity = g_Sun_Intensity;
        c.skyIntensity = g_IBL_SkyIntensity;
        c.aoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].aoIndex;
        memcpy(m_Light.cbMapped[m_FrameInFlightIdx], &c, sizeof(c));

        cmd->OMSetRenderTargets(1, &m_Light.rtvHandles[m_FrameInFlightIdx], false, nullptr);
        cmd->SetPipelineState(m_Light.pso.Get());
        cmd->SetGraphicsRootSignature(m_Light.rootSig.Get());
        cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
        cmd->SetGraphicsRootConstantBufferView(0, m_Light.cb[m_FrameInFlightIdx]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    cmd->RSSetViewports(1, &m_PresentViewport);
    cmd->RSSetScissorRects(1, &m_PresentRect);
}

void Renderer::Pass_FSR(const ComPtr<ID3D12GraphicsCommandList7>& cmd, f32 jitterNormX, f32 jitterNormY, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();

    auto EnsureFsrState = [&](u32 idx, D3D12_RESOURCE_STATES desired) {
        if (m_Fsr.outputState[idx] != desired)
        {
            Barrier(cmd, m_Fsr.outputs[idx], m_Fsr.outputState[idx], desired);
            m_Fsr.outputState[idx] = desired;
        }
    };

    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    EnsureFsrState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_FrameIndex > 0)
    {
        u32 prevIdx = (m_FrameInFlightIdx + IE_Constants::frameInFlightCount - 1) % IE_Constants::frameInFlightCount;
        EnsureFsrState(prevIdx, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "FSR");
    {
        ffx::DispatchDescUpscale d{};

        FfxApiResource hdr{};
        hdr.resource = m_Light.hdrRt[m_FrameInFlightIdx].Get();
        hdr.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 0, 0};
        hdr.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
        d.color = hdr;

        FfxApiResource depth{};
        depth.resource = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
        depth.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R32_FLOAT, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 0, 0};
        depth.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        d.depth = depth;

        FfxApiResource motion{};
        motion.resource = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.Get();
        motion.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 0, 0};
        motion.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        d.motionVectors = motion;

        FfxApiResource out{};
        out.resource = m_Fsr.outputs[m_FrameInFlightIdx].Get();
        out.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, m_Fsr.presentSize.x, m_Fsr.presentSize.y, 1, 1, 0, 0};
        out.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        d.output = out;

        d.commandList = cmd.Get();
        d.jitterOffset.x = jitterNormX;
        d.jitterOffset.y = jitterNormY;
        d.cameraFovAngleVertical = IE_ToRadians(g_Camera_FOV);
        d.cameraNear = cameraFrameData.znearfar.y;
        d.cameraFar = cameraFrameData.znearfar.x;
        d.motionVectorScale.x = static_cast<f32>(m_Fsr.renderSize.x);
        d.motionVectorScale.y = static_cast<f32>(m_Fsr.renderSize.y);
        d.frameTimeDelta = Window::GetFrameTimeMs();
        d.renderSize.width = m_Fsr.renderSize.x;
        d.renderSize.height = m_Fsr.renderSize.y;
        d.preExposure = 1.0f;

        IE_Assert(ffx::Dispatch(m_Fsr.context, d) == ffx::ReturnCode::Ok);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    UAVBarrier(cmd, m_Fsr.outputs[m_FrameInFlightIdx]);
    EnsureFsrState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Renderer::Pass_Histogram(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();
    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Histogram Pass");
    {
        ClearConstants clr;
        clr.bufferIndex = m_Histo.histogramBuffer->uavIndex;
        clr.numElements = m_Histo.histogramBuffer->numElements;
        cmd->SetPipelineState(m_Histo.clearUintPso.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_Histo.clearUintRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(clr) / 4, &clr, 0);
        cmd->Dispatch(IE_DivRoundUp(m_Histo.histogramBuffer->numElements, 64), 1, 1);

        UAVBarrier(cmd, m_Histo.histogramBuffer->buffer);

        HistogramConstants hc;
        hc.hdrTextureIndex = m_Fsr.srvIdx[m_FrameInFlightIdx];
        hc.minLogLum = g_AutoExposure_MinLogLum;
        hc.maxLogLum = g_AutoExposure_MaxLogLum;
        hc.numBuckets = m_Histo.numBuckets;
        hc.histogramBufferIndex = m_Histo.histogramBuffer->uavIndex;
        hc.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
        cmd->SetPipelineState(m_Histo.histogramPso.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_Histo.histogramRootSig.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(hc) / 4, &hc, 0);
        cmd->Dispatch(IE_DivRoundUp(m_Fsr.renderSize.x, 16), IE_DivRoundUp(m_Fsr.renderSize.y, 16), 1);

        UAVBarrier(cmd, m_Histo.histogramBuffer->buffer);

        ExposureConstants ec;
        ec.numBuckets = m_Histo.numBuckets;
        ec.totalPixels = m_Fsr.renderSize.x * m_Fsr.renderSize.y;
        ec.targetPct = g_AutoExposure_TargetPct;
        ec.lowReject = g_AutoExposure_LowReject;
        ec.highReject = g_AutoExposure_HighReject;
        ec.key = g_AutoExposure_Key;
        ec.minLogLum = g_AutoExposure_MinLogLum;
        ec.maxLogLum = g_AutoExposure_MaxLogLum;
        ec.histogramBufferIndex = m_Histo.histogramBuffer->srvIndex;
        ec.exposureBufferIndex = m_Histo.exposureBuffer->uavIndex;
        cmd->SetPipelineState(m_Histo.exposurePso.Get());
        cmd->SetComputeRootSignature(m_Histo.exposureRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(ec) / 4, &ec, 0);
        cmd->Dispatch(1, 1, 1);

        UAVBarrier(cmd, m_Histo.exposureBuffer->buffer);

        AdaptExposureConstants ac;
        ac.exposureBufferIndex = m_Histo.exposureBuffer->srvIndex;
        ac.adaptedExposureBufferIndex = m_Histo.adaptExposureBuffer->uavIndex;
        ac.dt = Window::GetFrameTimeMs() / 1000.f;
        ac.tauBright = g_AutoExposure_TauBright;
        ac.tauDark = g_AutoExposure_TauDark;
        ac.clampMin = g_AutoExposure_ClampMin;
        ac.clampMax = g_AutoExposure_ClampMax;
        cmd->SetPipelineState(m_Histo.adaptExposurePso.Get());
        cmd->SetComputeRootSignature(m_Histo.adaptExposureRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(ac) / 4, &ac, 0);
        cmd->Dispatch(1, 1, 1);

        UAVBarrier(cmd, m_Histo.adaptExposureBuffer->buffer);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_Tonemap(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Tone Mapping");
    {
        Barrier(cmd, m_Tonemap.sdrRt[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        TonemapConstants t;
        t.srvIndex = m_Fsr.srvIdx[m_FrameInFlightIdx];
        t.samplerIndex = m_LinearSamplerIdx;
        t.whitePoint = g_ToneMapping_WhitePoint;
        t.contrast = g_ToneMapping_Contrast;
        t.saturation = g_ToneMapping_Saturation;
        t.adaptExposureBufferIndex = m_Histo.adaptExposureBuffer->srvIndex;

        cmd->OMSetRenderTargets(1, &m_Tonemap.rtvHandles[m_FrameInFlightIdx], false, nullptr);
        cmd->SetPipelineState(m_Tonemap.pso.Get());
        cmd->SetGraphicsRootSignature(m_Tonemap.rootSig.Get());
        cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(t) / 4, &t, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_ImGui(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "ImGui");
    {
        Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        static ImGui_TimingRaw raw[128];
        static ImGui_TimingSmooth smt[128];

        u32 nRaw = m_GpuTimingState.lastCount;
        for (u32 i = 0; i < nRaw; ++i)
        {
            raw[i].name = m_GpuTimingState.last[i].name;
            raw[i].ms = m_GpuTimingState.last[i].ms;
        }

        u32 nSmt = m_GpuTimingState.smoothCount;
        for (u32 i = 0; i < nSmt; ++i)
        {
            smt[i].name = m_GpuTimingState.smooth[i].name;
            smt[i].value = m_GpuTimingState.smooth[i].value;
        }

        ImGui_FrameStats frameStats;
        frameStats.fps = static_cast<u32>(Window::GetFPS());
        frameStats.cameraPos[0] = cameraFrameData.position.x;
        frameStats.cameraPos[1] = cameraFrameData.position.y;
        frameStats.cameraPos[2] = cameraFrameData.position.z;

        const Raytracing::ShadowPassOutput& rtOut = Raytracing::GetInstance().GetShadowPassOutput();

        ImGui_RenderParams rp;
        rp.cmd = cmd.Get();
        rp.rtv = m_Tonemap.rtvHandles[m_FrameInFlightIdx];
        rp.rtvResource = m_Tonemap.sdrRt[m_FrameInFlightIdx].Get();
        rp.gbufferAlbedo = m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.Get();
        rp.gbufferNormal = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.Get();
        rp.gbufferMaterial = m_GBuf.gbuffers[m_FrameInFlightIdx].material.Get();
        rp.gbufferMotion = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.Get();
        rp.gbufferAO = m_GBuf.gbuffers[m_FrameInFlightIdx].ao.Get();
        rp.depth = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
        rp.rtShadows = rtOut.resource.Get();
        rp.ssao = m_Ssao.texture.Get();
        rp.renderWidth = m_Fsr.renderSize.x;
        rp.renderHeight = m_Fsr.renderSize.y;
        rp.frame = frameStats;
        rp.timingsRaw = raw;
        rp.timingsRawCount = nRaw;
        rp.timingsSmooth = smt;
        rp.timingsSmoothCount = nSmt;
        ImGui_Render(rp);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::EndFrame(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    if (frameData.gpuTimers.nextIdx)
    {
        cmd->ResolveQueryData(frameData.gpuTimers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, frameData.gpuTimers.nextIdx, frameData.gpuTimers.readback.Get(), 0);
    }

    IE_Check(cmd->Close());
    ID3D12CommandList* pCmdList = cmd.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmdList);
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), frameData.frameFenceValue));

    IE_Check(m_Swapchain->Present(0, 0));
    m_FrameIndex++;
}

void Renderer::CreateDevice()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();

        const CommandLineArguments& args = GetCommandLineArguments();
        if (args.gpuValidation)
        {
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(true);
            }
        }
    }
    m_Debug = debug;

#ifdef _DEBUG
    u32 factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#else
    u32 factoryFlags = 0;
#endif
    IE_Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_DxgiFactory)));

    ComPtr<IDXGIFactory6> factory6;
    IE_Check(m_DxgiFactory.As(&factory6));

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;

    for (u32 i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc;
        IE_Check(adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), featureLevel, __uuidof(ID3D12Device), nullptr)))
        {
            m_Adapter = adapter;
            break;
        }
    }
    IE_Check(D3D12CreateDevice(m_Adapter.Get(), featureLevel, IID_PPV_ARGS(&m_Device)));
#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_Device.As(&infoQueue)))
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
    }
#endif
}

void Renderer::CreateAllocator()
{
    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED | D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;
    allocatorDesc.pDevice = m_Device.Get();
    allocatorDesc.pAdapter = m_Adapter.Get();
    IE_Check(D3D12MA::CreateAllocator(&allocatorDesc, &m_Allocator));
}

void Renderer::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    commandQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    IE_Check(m_Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_CommandQueue)));
}

void Renderer::CreateSwapchain()
{
    const Window& window = Window::GetInstance();

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
    swapchainDesc.Width = m_Fsr.presentSize.x;
    swapchainDesc.Height = m_Fsr.presentSize.y;
    swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = IE_Constants::frameInFlightCount;
    swapchainDesc.Scaling = DXGI_SCALING_NONE;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> tempSwapchain;
    IE_Check(m_DxgiFactory->CreateSwapChainForHwnd(m_CommandQueue.Get(), window.GetHwnd(), &swapchainDesc, nullptr, nullptr, tempSwapchain.ReleaseAndGetAddressOf()));
    IE_Check(m_DxgiFactory->MakeWindowAssociation(window.GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
    IE_Check(tempSwapchain->QueryInterface(IID_PPV_ARGS(&m_Swapchain)));
}

void Renderer::CreateFrameSynchronizationFences()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_AllFrameData[i].frameFence)));
    }
}

void Renderer::CreateCommands()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_AllFrameData[i].commandAllocator)));
        IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_AllFrameData[i].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_AllFrameData[i].cmd)));
        IE_Check(m_AllFrameData[i].cmd->SetName(L"Main command list"));
        m_AllFrameData[i].cmd->Close();
    }
}

void Renderer::CreateRTVs()
{
    // SDR
    D3D12_DESCRIPTOR_HEAP_DESC sdrRtvHeapDesc{};
    sdrRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    sdrRtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
    IE_Check(m_Device->CreateDescriptorHeap(&sdrRtvHeapDesc, IID_PPV_ARGS(&m_Tonemap.rtvHeap)));
    IE_Check(m_Tonemap.rtvHeap->SetName(L"SDR Color Target : Heap"));

    D3D12_RENDER_TARGET_VIEW_DESC sdrRtvDesc{};
    sdrRtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sdrRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_Tonemap.sdrRt[i])));
        IE_Check(m_Tonemap.sdrRt[i]->SetName(L"SDR Color Target"));

        m_Tonemap.rtvHandles[i] = m_Tonemap.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_Tonemap.rtvHandles[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_Device->CreateRenderTargetView(m_Tonemap.sdrRt[i].Get(), &sdrRtvDesc, m_Tonemap.rtvHandles[i]);
    }

    // HDR
    D3D12_DESCRIPTOR_HEAP_DESC hdrRtvHeapDesc{};
    hdrRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hdrRtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;

    IE_Check(m_Device->CreateDescriptorHeap(&hdrRtvHeapDesc, IID_PPV_ARGS(&m_Light.rtvHeap)));
    IE_Check(m_Light.rtvHeap->SetName(L"HDR Float Color Target : Heap"));

    CD3DX12_HEAP_PROPERTIES hdrHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        CD3DX12_RESOURCE_DESC hdrRtvDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        IE_Check(m_Device->CreateCommittedResource(&hdrHeapProps, D3D12_HEAP_FLAG_NONE, &hdrRtvDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&m_Light.hdrRt[i])));
        IE_Check(m_Light.hdrRt[i]->SetName(L"HDR Float Color Target"));

        D3D12_RENDER_TARGET_VIEW_DESC hdrRtvViewDesc{};
        hdrRtvViewDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdrRtvViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_Light.rtvHandles[i] = m_Light.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_Light.rtvHandles[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_Device->CreateRenderTargetView(m_Light.hdrRt[i].Get(), &hdrRtvViewDesc, m_Light.rtvHandles[i]);
    }
}

void Renderer::CreateDSV()
{
    D3D12_DESCRIPTOR_HEAP_DESC descriptorDsvHeapDesc{};
    descriptorDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    descriptorDsvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorDsvHeapDesc, IID_PPV_ARGS(&m_DepthPre.dsvHeap)));
    IE_Check(m_DepthPre.dsvHeap->SetName(L"Depth/Stencil : Heap"));
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 0.0f;

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, m_DepthPre.dsvAllocs[i].ReleaseAndGetAddressOf(),
                                             IID_PPV_ARGS(&m_DepthPre.dsvs[i])));
        IE_Check(m_DepthPre.dsvs[i]->SetName(L"Depth/Stencil : DSV"));

        m_DepthPre.dsvHandles[i] = m_DepthPre.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_DepthPre.dsvHandles[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        m_Device->CreateDepthStencilView(m_DepthPre.dsvs[i].Get(), &dsvDesc, m_DepthPre.dsvHandles[i]);
    }
}

void Renderer::SetRenderAndPresentSize()
{
    m_Fsr.presentSize = Window::GetInstance().GetResolution();

    ffx::QueryDescUpscaleGetRenderResolutionFromQualityMode query{};
    query.displayWidth = m_Fsr.presentSize.x;
    query.displayHeight = m_Fsr.presentSize.y;
    query.qualityMode = IE_Constants::upscalingMode;
    query.pOutRenderWidth = &m_Fsr.renderSize.x;
    query.pOutRenderHeight = &m_Fsr.renderSize.y;
    IE_Assert(ffx::Query(query) == ffx::ReturnCode::Ok);

    m_PresentViewport.TopLeftX = 0.f;
    m_PresentViewport.TopLeftY = 0.f;
    m_PresentViewport.Width = static_cast<f32>(m_Fsr.presentSize.x);
    m_PresentViewport.Height = static_cast<f32>(m_Fsr.presentSize.y);
    m_PresentViewport.MinDepth = 0.f;
    m_PresentViewport.MaxDepth = 1.f;

    m_PresentRect.left = 0;
    m_PresentRect.top = 0;
    m_PresentRect.right = static_cast<i32>(m_Fsr.presentSize.x);
    m_PresentRect.bottom = static_cast<i32>(m_Fsr.presentSize.y);

    m_RenderViewport.TopLeftX = 0.f;
    m_RenderViewport.TopLeftY = 0.f;
    m_RenderViewport.Width = static_cast<f32>(m_Fsr.renderSize.x);
    m_RenderViewport.Height = static_cast<f32>(m_Fsr.renderSize.y);
    m_RenderViewport.MinDepth = 0.f;
    m_RenderViewport.MaxDepth = 1.f;

    m_RenderRect.left = 0;
    m_RenderRect.top = 0;
    m_RenderRect.right = static_cast<i32>(m_Fsr.renderSize.x);
    m_RenderRect.bottom = static_cast<i32>(m_Fsr.renderSize.y);
}

void Renderer::CreateGPUTimers()
{
    IE_Check(m_CommandQueue->GetTimestampFrequency(&m_GpuTimingState.timestampFrequency));

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        constexpr u32 maxTimestamps = 256;

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = maxTimestamps;
        IE_Check(m_Device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_AllFrameData[i].gpuTimers.heap)));

        CD3DX12_RESOURCE_DESC rb = CD3DX12_RESOURCE_DESC::Buffer(maxTimestamps * sizeof(u64));
        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_READBACK);
        IE_Check(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_AllFrameData[i].gpuTimers.readback)));

        m_AllFrameData[i].gpuTimers.nextIdx = 0;
        m_AllFrameData[i].gpuTimers.passCount = 0;
    }
}

void Renderer::CreateFSRPassResources()
{
    ffx::CreateBackendDX12Desc backendDesc{};
    backendDesc.device = m_Device.Get();

    ffx::CreateContextDescUpscale createUpscaling{};
    createUpscaling.maxRenderSize = {m_Fsr.renderSize.x, m_Fsr.renderSize.y};
    createUpscaling.maxUpscaleSize = {m_Fsr.presentSize.x, m_Fsr.presentSize.y};
    createUpscaling.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    IE_Assert(ffx::CreateContext(m_Fsr.context, nullptr, createUpscaling, backendDesc) == ffx::ReturnCode::Ok);

    ffx::QueryDescUpscaleGetJitterPhaseCount getJitterPhaseDesc{};
    getJitterPhaseDesc.displayWidth = m_Fsr.presentSize.x;
    getJitterPhaseDesc.renderWidth = m_Fsr.renderSize.x;
    getJitterPhaseDesc.pOutPhaseCount = &m_Fsr.jitterPhaseCount;
    IE_Assert(ffx::Query(m_Fsr.context, getJitterPhaseDesc) == ffx::ReturnCode::Ok);

    ffx::QueryDescUpscaleGetJitterOffset getJitterOffsetDesc{};
    getJitterOffsetDesc.index = m_Fsr.jitterIndex;
    getJitterOffsetDesc.phaseCount = m_Fsr.jitterPhaseCount;
    getJitterOffsetDesc.pOutX = &m_Fsr.jitterX;
    getJitterOffsetDesc.pOutY = &m_Fsr.jitterY;
    IE_Assert(ffx::Query(m_Fsr.context, getJitterOffsetDesc) == ffx::ReturnCode::Ok);

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        CD3DX12_RESOURCE_DESC fsrDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_Fsr.presentSize.x, m_Fsr.presentSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        IE_Check(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &fsrDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_Fsr.outputs[i])));
        IE_Check(m_Fsr.outputs[i]->SetName(L"FSR Output"));

        D3D12_SHADER_RESOURCE_VIEW_DESC fsrSrvDesc{};
        fsrSrvDesc.Format = fsrDesc.Format;
        fsrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        fsrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        fsrSrvDesc.Texture2D.MipLevels = 1;
        m_Fsr.srvIdx[i] = m_BindlessHeaps.CreateSRV(m_Fsr.outputs[i].Get(), fsrSrvDesc);
    }
}

void Renderer::CreateDepthPrePassResources(const Vector<WString>& globalDefines)
{
    m_GBuf.amplificationShader = LoadShader(IE_SHADER_TYPE_AMPLIFICATION, L"asBasic.hlsl", {globalDefines});
    m_GBuf.meshShader = LoadShader(IE_SHADER_TYPE_MESH, L"msBasic.hlsl", {globalDefines});

    CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
    ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    // Opaque
    {
        IE_Check(m_Device->CreateRootSignature(0, m_GBuf.meshShader.bytecode.pShaderBytecode, m_GBuf.meshShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_DepthPre.opaqueRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.opaqueRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader.bytecode.BytecodeLength > 0 ? m_GBuf.amplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        depthDesc.MS = m_GBuf.meshShader.bytecode;
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPre.opaquePSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPre.opaquePSO[CullMode_None])));
    }

    // Alpha Tested
    {
        m_DepthPre.alphaTestShader = LoadShader(IE_SHADER_TYPE_PIXEL, L"psAlphaTest.hlsl", {globalDefines});

        IE_Check(m_Device->CreateRootSignature(0, m_DepthPre.alphaTestShader.bytecode.pShaderBytecode, m_DepthPre.alphaTestShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_DepthPre.alphaTestRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.alphaTestRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader.bytecode.BytecodeLength > 0 ? m_GBuf.amplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        depthDesc.MS = m_GBuf.meshShader.bytecode;
        depthDesc.PS = m_DepthPre.alphaTestShader.bytecode;
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPre.alphaTestPSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPre.alphaTestPSO[CullMode_None])));
    }
}

void Renderer::CreateGBufferPassResources(const Vector<WString>& globalDefines)
{
    DXGI_FORMAT formats[GBuffer::targetCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM, // Albedo
        DXGI_FORMAT_R16G16_FLOAT,   // Normal
        DXGI_FORMAT_R8G8_UNORM,     // Material
        DXGI_FORMAT_R16G16_FLOAT,   // Motion vector
        DXGI_FORMAT_R8_UNORM,       // AO
    };
    const wchar_t* rtvNames[GBuffer::targetCount] = {L"GBuffer Albedo", L"GBuffer Normal", L"GBuffer Material", L"GBuffer Motion Vector", L"GBuffer AO"};

    m_GBuf.pixelShaders[AlphaMode_Opaque] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", globalDefines);

    Vector<WString> blendDefines = globalDefines;
    blendDefines.push_back(L"ENABLE_BLEND");
    m_GBuf.pixelShaders[AlphaMode_Blend] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", blendDefines);

    Vector<WString> maskDefines = globalDefines;
    maskDefines.push_back(L"ENABLE_ALPHA_TEST");
    m_GBuf.pixelShaders[AlphaMode_Mask] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", maskDefines);

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        GBuffer& gbuff = m_GBuf.gbuffers[i];
        ComPtr<ID3D12Resource>* targets[GBuffer::targetCount] = {&gbuff.albedo, &gbuff.normal, &gbuff.material, &gbuff.motionVector, &gbuff.ao};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = GBuffer::targetCount;
        IE_Check(m_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gbuff.rtvHeap)));

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gbuff.rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (u32 t = 0; t < GBuffer::targetCount; ++t)
        {
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[t], m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE clearValue = {formats[t], {0.f, 0.f, 0.f, 0.f}};
            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

            IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(targets[t]->GetAddressOf())));
            IE_Check(targets[t]->Get()->SetName(rtvNames[t]));

            m_Device->CreateRenderTargetView(targets[t]->Get(), nullptr, rtvHandle);
            m_GBuf.rtvHandles[i][t] = rtvHandle;

            rtvHandle.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }

    for (AlphaMode alphaMode = AlphaMode_Opaque; alphaMode < AlphaMode_Count; alphaMode = static_cast<AlphaMode>(alphaMode + 1))
    {
        IE_Check(m_Device->CreateRootSignature(0, m_GBuf.meshShader.bytecode.pShaderBytecode, m_GBuf.meshShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_GBuf.rootSigs[alphaMode])));

        CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
        ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC msDesc{};
        msDesc.pRootSignature = m_GBuf.rootSigs[alphaMode].Get();
        msDesc.AS = m_GBuf.amplificationShader.bytecode.BytecodeLength > 0 ? m_GBuf.amplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        msDesc.MS = m_GBuf.meshShader.bytecode;
        msDesc.PS = m_GBuf.pixelShaders[alphaMode].bytecode;
        msDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        msDesc.SampleMask = UINT_MAX;
        msDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        msDesc.DepthStencilState = ds;
        msDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        msDesc.NumRenderTargets = GBuffer::targetCount;
        msDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        msDesc.SampleDesc = DefaultSampleDesc();
        for (u32 i = 0; i < GBuffer::targetCount; ++i)
        {
            msDesc.RTVFormats[i] = formats[i];
        }

        // Cull
        {
            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(msDesc);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBuf.psos[alphaMode][CullMode_Back])));
        }

        // No cull
        {
            D3DX12_MESH_SHADER_PIPELINE_STATE_DESC noCull = msDesc;
            D3D12_RASTERIZER_DESC rast = noCull.RasterizerState;
            rast.CullMode = D3D12_CULL_MODE_NONE;
            noCull.RasterizerState = rast;

            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(noCull);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBuf.psos[alphaMode][CullMode_None])));
        }
    }
}

void Renderer::CreateLightingPassResources(const Vector<WString>& globalDefines)
{
    m_Light.shader = LoadShader(IE_SHADER_TYPE_PIXEL, L"psLighting.hlsl", {globalDefines});
    ComPtr<IDxcBlob> vsFullscreen = CompileShader(IE_SHADER_TYPE_VERTEX, L"vsFullscreen.hlsl", {});

    IE_Check(m_Device->CreateRootSignature(0, m_Light.shader.bytecode.pShaderBytecode, m_Light.shader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_Light.rootSig)));

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_Light.rootSig.Get();
    psoDesc.VS.pShaderBytecode = vsFullscreen->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vsFullscreen->GetBufferSize();
    psoDesc.PS.pShaderBytecode = m_Light.shader.bytecode.pShaderBytecode;
    psoDesc.PS.BytecodeLength = m_Light.shader.bytecode.BytecodeLength;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    psoDesc.SampleDesc = DefaultSampleDesc();
    IE_Check(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_Light.pso)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv2D{};
    srv2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv2D.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv2D.Texture2D.MipLevels = 1;

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(LightingPassConstants) + 255 & ~255);

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        srv2D.Format = m_GBuf.gbuffers[i].albedo->GetDesc().Format;
        m_GBuf.gbuffers[i].albedoIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].albedo, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].normal->GetDesc().Format;
        m_GBuf.gbuffers[i].normalIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].normal, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].material->GetDesc().Format;
        m_GBuf.gbuffers[i].materialIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].material, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].motionVector->GetDesc().Format;
        m_GBuf.gbuffers[i].motionVectorIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].motionVector, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].ao->GetDesc().Format;
        m_GBuf.gbuffers[i].aoIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].ao, srv2D);

        IE_Check(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_Light.cb[i])));
        IE_Check(m_Light.cb[i]->SetName(L"LightingPassConstants"));
        m_Light.cb[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_Light.cbMapped[i]));
    }
}

void Renderer::CreateHistogramPassResources(const Vector<WString>&)
{
    m_Histo.histogramBuffer = CreateStructuredBuffer(m_Histo.numBuckets * sizeof(u32), sizeof(u32), L"Histogram");

    // Clear pass
    m_Histo.clearUintShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csClearUint.hlsl", {});

    IE_Check(m_Device->CreateRootSignature(0, m_Histo.clearUintShader.bytecode.pShaderBytecode, m_Histo.clearUintShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_Histo.clearUintRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC clearUintPsoDesc{};
    clearUintPsoDesc.pRootSignature = m_Histo.clearUintRootSig.Get();
    clearUintPsoDesc.CS.pShaderBytecode = m_Histo.clearUintShader.bytecode.pShaderBytecode;
    clearUintPsoDesc.CS.BytecodeLength = m_Histo.clearUintShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&clearUintPsoDesc, IID_PPV_ARGS(&m_Histo.clearUintPso)));

    // Histogram pass
    m_Histo.histogramShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csHistogram.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.histogramShader.bytecode.pShaderBytecode, m_Histo.histogramShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_Histo.histogramRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC histogramPsoDesc{};
    histogramPsoDesc.pRootSignature = m_Histo.histogramRootSig.Get();
    histogramPsoDesc.CS.pShaderBytecode = m_Histo.histogramShader.bytecode.pShaderBytecode;
    histogramPsoDesc.CS.BytecodeLength = m_Histo.histogramShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&histogramPsoDesc, IID_PPV_ARGS(&m_Histo.histogramPso)));

    // Exposure pass
    m_Histo.exposureBuffer = CreateStructuredBuffer(1 * sizeof(f32), sizeof(f32), L"Exposure");
    m_Histo.exposureShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csExposure.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.exposureShader.bytecode.pShaderBytecode, m_Histo.exposureShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_Histo.exposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC exposurePsoDesc{};
    exposurePsoDesc.pRootSignature = m_Histo.exposureRootSig.Get();
    exposurePsoDesc.CS.pShaderBytecode = m_Histo.exposureShader.bytecode.pShaderBytecode;
    exposurePsoDesc.CS.BytecodeLength = m_Histo.exposureShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&exposurePsoDesc, IID_PPV_ARGS(&m_Histo.exposurePso)));

    // Adapt exposure pass
    m_Histo.adaptExposureBuffer = CreateStructuredBuffer(1 * sizeof(f32), sizeof(f32), L"Adapt Exposure");
    m_Histo.adaptExposureShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csAdaptExposure.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.adaptExposureShader.bytecode.pShaderBytecode, m_Histo.adaptExposureShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_Histo.adaptExposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC adaptExposurePsoDesc{};
    adaptExposurePsoDesc.pRootSignature = m_Histo.adaptExposureRootSig.Get();
    adaptExposurePsoDesc.CS.pShaderBytecode = m_Histo.adaptExposureShader.bytecode.pShaderBytecode;
    adaptExposurePsoDesc.CS.BytecodeLength = m_Histo.adaptExposureShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&adaptExposurePsoDesc, IID_PPV_ARGS(&m_Histo.adaptExposurePso)));
}

void Renderer::CreateToneMapPassResources(const Vector<WString>& globalDefines)
{
    ComPtr<IDxcBlob> vsFullscreen = CompileShader(IE_SHADER_TYPE_VERTEX, L"vsFullscreen.hlsl", {globalDefines});
    ComPtr<IDxcBlob> psTonemap = CompileShader(IE_SHADER_TYPE_PIXEL, L"psTonemap.hlsl", {globalDefines});
    CD3DX12_SHADER_BYTECODE psTonemapBytecode = CD3DX12_SHADER_BYTECODE(psTonemap->GetBufferPointer(), psTonemap->GetBufferSize());
    IE_Check(m_Device->CreateRootSignature(0, psTonemapBytecode.pShaderBytecode, psTonemapBytecode.BytecodeLength, IID_PPV_ARGS(&m_Tonemap.rootSig)));

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_Tonemap.rootSig.Get();
    psoDesc.VS.pShaderBytecode = vsFullscreen->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vsFullscreen->GetBufferSize();
    psoDesc.PS.pShaderBytecode = psTonemap->GetBufferPointer();
    psoDesc.PS.BytecodeLength = psTonemap->GetBufferSize();
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc = DefaultSampleDesc();
    IE_Check(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_Tonemap.pso)));
}

void Renderer::CreateSSAOResources(const Vector<WString>&)
{
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, m_Fsr.renderSize.x, m_Fsr.renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Ssao.texture)));
    IE_Check(m_Ssao.texture->SetName(L"SSAO Texture"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Ssao.uavIdx = m_BindlessHeaps.CreateUAV(m_Ssao.texture.Get(), uavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Ssao.srvIdx = m_BindlessHeaps.CreateSRV(m_Ssao.texture.Get(), srvDesc);

    ComPtr<IDxcBlob> cs = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csSSAO.hlsl", {});

    IE_Check(m_Device->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), IID_PPV_ARGS(&m_Ssao.rootSig)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_Ssao.rootSig.Get();
    psoDesc.CS.pShaderBytecode = cs->GetBufferPointer();
    psoDesc.CS.BytecodeLength = cs->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_Ssao.pso)));
}

void Renderer::CreateEnvMapResources(const WString& envName)
{
    m_Env.name = envName;

    WString basePath = L"data/textures/" + envName;

    ResourceUploadBatch batch(m_Device.Get());
    batch.Begin();

    IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/envMap.dds").c_str(), &m_Env.envCube, false, 0, nullptr, nullptr));
    IE_Check(m_Env.envCube->SetName(L"EnvCubeMap"));

    IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/diffuseIBL.dds").c_str(), &m_Env.diffuseIBL, false, 0, nullptr, nullptr));
    IE_Check(m_Env.diffuseIBL->SetName(L"DiffuseIBL"));

    IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/specularIBL.dds").c_str(), &m_Env.specularIBL, false, 0, nullptr, nullptr));
    IE_Check(m_Env.specularIBL->SetName(L"SpecularIBL"));

    IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, L"data/textures/BRDF_LUT.dds", &m_Env.brdfLut, false, 0, nullptr, nullptr));
    IE_Check(m_Env.brdfLut->SetName(L"BrdfLut"));

    batch.End(m_CommandQueue.Get()).wait();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    srv.Format = DXGI_FORMAT_BC6H_UF16;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = UINT32_MAX;
    m_Env.envSrvIdx = m_BindlessHeaps.CreateSRV(m_Env.envCube, srv);

    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_Env.diffuseSrvIdx = m_BindlessHeaps.CreateSRV(m_Env.diffuseIBL, srv);

    srv.Format = DXGI_FORMAT_BC6H_UF16;
    m_Env.specularSrvIdx = m_BindlessHeaps.CreateSRV(m_Env.specularIBL, srv);

    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = UINT32_MAX;
    m_Env.brdfSrvIdx = m_BindlessHeaps.CreateSRV(m_Env.brdfLut, srv);
}

void Renderer::LoadScene()
{
    const CommandLineArguments& args = GetCommandLineArguments();
    String sceneFile = args.sceneFile.empty() ? "San-Miguel" : args.sceneFile;

    // Camera config stays in renderer
    Camera& camera = Camera::GetInstance();
    camera.LoadSceneConfig(sceneFile);

    // GPU-side scene construction handled by SceneLoader
    SceneLoader::Load(*this, sceneFile);
}

void Renderer::WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue)
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        IE_Check(fence->SetEventOnCompletion(fenceValue, event));
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
    fenceValue++;
}

Renderer::PerFrameData& Renderer::GetCurrentFrameData()
{
    return m_AllFrameData[m_Swapchain->GetCurrentBackBufferIndex()];
}

SharedPtr<Buffer> Renderer::CreateStructuredBuffer(u32 sizeInBytes, u32 strideInBytes, const WString& name, D3D12_HEAP_TYPE heapType)
{
    SharedPtr<Buffer> buffer = IE_MakeSharedPtr<Buffer>();

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType;

    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, buffer->allocation.ReleaseAndGetAddressOf(),
                                         IID_PPV_ARGS(buffer->buffer.ReleaseAndGetAddressOf())));
    IE_Check(buffer->buffer->SetName(name.data()));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = sizeInBytes / strideInBytes;
    srvDesc.Buffer.StructureByteStride = strideInBytes;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = sizeInBytes / strideInBytes;
    uavDesc.Buffer.StructureByteStride = strideInBytes;

    buffer->srvIndex = m_BindlessHeaps.CreateSRV(buffer->buffer, srvDesc);
    buffer->uavIndex = m_BindlessHeaps.CreateUAV(buffer->buffer, uavDesc);
    buffer->numElements = srvDesc.Buffer.NumElements;
    return buffer;
}

SharedPtr<Buffer> Renderer::CreateBytesBuffer(u32 numElements, const WString& name, D3D12_HEAP_TYPE heapType)
{
    SharedPtr<Buffer> buffer = IE_MakeSharedPtr<Buffer>();

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(numElements, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, buffer->allocation.ReleaseAndGetAddressOf(),
                                         IID_PPV_ARGS(buffer->buffer.ReleaseAndGetAddressOf())));
    IE_Check(buffer->buffer->SetName(name.data()));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = numElements / 4; // raw is in 32-bit words
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = numElements / 4; // raw is in 32-bit words
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

    buffer->srvIndex = m_BindlessHeaps.CreateSRV(buffer->buffer, srvDesc);
    buffer->uavIndex = m_BindlessHeaps.CreateUAV(buffer->buffer, uavDesc);
    buffer->numElements = numElements / 4;
    return buffer;
}

Shader Renderer::LoadShader(const ShaderType type, const WString& filename, const Vector<WString>& defines)
{
    Vector<WString> definesStrings;
    for (const WString& define : defines)
    {
        definesStrings.push_back(L"-D" + define);
    }

    ComPtr<IDxcBlob> result = CompileShader(type, filename, definesStrings);

    Shader dxShader;
    dxShader.bytecode = CD3DX12_SHADER_BYTECODE(result->GetBufferPointer(), result->GetBufferSize());
    dxShader.filename = filename;
    dxShader.defines = defines;
    return dxShader;
}

void Renderer::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> uploadBuffer;
    AllocateUploadBuffer(data, sizeInBytes, offsetInBytes, uploadBuffer, allocation, L"SetBufferData/TempUploadBuffer");

    Barrier(cmd, buffer->buffer, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(buffer->buffer.Get(), offsetInBytes, uploadBuffer.Get(), 0, sizeInBytes);
    Barrier(cmd, buffer->buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);

    m_InFlightUploads.push_back({uploadBuffer, allocation});
}

void Renderer::SetResourceBufferData(const ComPtr<ID3D12Resource>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    u8* mappedData;
    IE_Check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)));
    memcpy(mappedData + offsetInBytes, data, sizeInBytes);
    buffer->Unmap(0, nullptr);
}

void Renderer::Barrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), stateBefore, stateAfter);
    cmd->ResourceBarrier(1, &barrier);
}

void Renderer::UAVBarrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource)
{
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource.Get());
    cmd->ResourceBarrier(1, &barrier);
}

void Renderer::AllocateUploadBuffer(const void* pData, u32 sizeInBytes, u32 offsetInBytes, ComPtr<ID3D12Resource>& resource, ComPtr<D3D12MA::Allocation>& allocation, const wchar_t* resourceName) const
{
    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS(&resource)));
    IE_Check(resource->SetName(resourceName));
    if (pData)
    {
        SetResourceBufferData(resource, pData, sizeInBytes, offsetInBytes);
    }
}

void Renderer::AllocateUAVBuffer(u32 sizeInBytes, ComPtr<ID3D12Resource>& resource, ComPtr<D3D12MA::Allocation>& allocation, D3D12_RESOURCE_STATES initialResourceState,
                                 const wchar_t* resourceName) const
{
    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, initialResourceState, nullptr, allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS(&resource)));
    IE_Check(resource->SetName(resourceName));
}

BindlessHeaps& Renderer::GetBindlessHeaps()
{
    return m_BindlessHeaps;
}

const ComPtr<ID3D12Device14>& Renderer::GetDevice() const
{
    return m_Device;
}

XMUINT2 Renderer::GetRenderSize() const
{
    return m_Fsr.renderSize;
}
