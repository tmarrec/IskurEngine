// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Renderer.h"

#include <D3D12MemAlloc.h>
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <ResourceUploadBatch.h>
#include <chrono>
#include <directx/d3dx12.h>
#include <dx12/ffx_api_dx12.hpp>
#include <execution>
#include <ffx_api.hpp>
#include <ffx_api_loader.h>
#include <ffx_upscale.hpp>

#include "Camera.h"
#include "Constants.h"
#include "ImGui.h"
#include "SceneLoader.h"
#include "common/Asserts.h"
#include "common/CommandLineArguments.h"
#include "common/IskurPackFormat.h"
#include "common/Types.h"
#include "common/math/MathUtils.h"
#include "meshoptimizer.h"
#include "window/Window.h"

#include <filesystem>

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

    // FSR
    IE_Assert(ffx::DestroyContext(m_UpscalingContext) == ffx::ReturnCode::Ok);

    // ImGui
    ImGui_Shutdown();
}

void Renderer::Render()
{
    // Get/Wait next frame
    m_FrameInFlightIdx = m_Swapchain->GetCurrentBackBufferIndex();
    PerFrameData& frameData = GetCurrentFrameData();
    WaitOnFence(frameData.frameFence, frameData.frameFenceValue);

    // Grab timings from the previous use of this frame slot
    GpuTimings_Collect(frameData.gpuTimers, m_GpuTimingState);
    GpuTimings_UpdateAverages(m_GpuTimingState, Window::GetFrameTimeMs(), g_Timing_AverageWindowMs);
    frameData.gpuTimers.passCount = 0;
    frameData.gpuTimers.nextIdx = 0;

    // Jittering
    m_JitterIndex = (m_JitterIndex + 1) % m_JitterPhaseCount;

    ffx::QueryDescUpscaleGetJitterOffset jo{};
    jo.index = m_JitterIndex;
    jo.phaseCount = m_JitterPhaseCount;
    jo.pOutX = &m_JitterX;
    jo.pOutY = &m_JitterY;
    IE_Assert(ffx::Query(m_UpscalingContext, jo) == ffx::ReturnCode::Ok);

    f32 jitterNormX = m_JitterX * 2.0f / static_cast<f32>(m_RenderSize.x);
    f32 jitterNormY = -m_JitterY * 2.0f / static_cast<f32>(m_RenderSize.y);

    Camera& camera = Camera::GetInstance();
    camera.ConfigurePerspective(Window::GetInstance().GetAspectRatio(), IE_ToRadians(g_Camera_FOV), IE_ToRadians(g_Camera_FrustumCullingFOV), 0.1f, jitterNormX, jitterNormY);

    // Sun dir (normalized)
    {
        const float cosE = cosf(g_Sun_Elevation);
        const float sinE = sinf(g_Sun_Elevation);
        const float cosA = cosf(g_Sun_Azimuth);
        const float sinA = sinf(g_Sun_Azimuth);
        XMVECTOR sun = XMVectorSet(cosE * cosA, sinE, cosE * sinA, 0.0f);
        sun = XMVector3Normalize(sun);
        XMStoreFloat3(&m_SunDir, sun);
    }

    XMFLOAT4X4 view = camera.GetViewMatrix();
    XMFLOAT4X4 projJ = camera.GetProjection();
    XMFLOAT4X4 projNoJ = camera.GetProjectionNoJitter();

    XMFLOAT4X4 prevViewProjNoJ = m_LastViewProjNoJ;

    XMMATRIX Mview = XMLoadFloat4x4(&view);
    XMMATRIX MprojNJ = XMLoadFloat4x4(&projNoJ);
    XMMATRIX MvpNJ = XMMatrixMultiply(Mview, MprojNJ);
    XMFLOAT4X4 viewProjNoJ{};
    XMStoreFloat4x4(&viewProjNoJ, MvpNJ);
    m_LastViewProjNoJ = viewProjNoJ;

    XMFLOAT2 zNearFar = camera.GetZNearFar();

    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = {m_BindlessHeaps.m_CbvSrvUavHeap.Get(), m_BindlessHeaps.m_SamplerHeap.Get()};

    // Constant Buffer
    XMFLOAT4X4 projCull = camera.GetFrustumCullingProjection();
    XMMATRIX MprojCull = XMLoadFloat4x4(&projCull);
    XMMATRIX MvpCull = XMMatrixMultiply(Mview, MprojCull);
    XMMATRIX MvpT = XMMatrixTranspose(MvpCull);
    XMFLOAT4X4 vp{};
    XMStoreFloat4x4(&vp, MvpT);

    // Frustum planes
    XMVECTOR r0 = XMVectorSet(vp._11, vp._12, vp._13, vp._14);
    XMVECTOR r1 = XMVectorSet(vp._21, vp._22, vp._23, vp._24);
    XMVECTOR r2 = XMVectorSet(vp._31, vp._32, vp._33, vp._34);
    XMVECTOR r3 = XMVectorSet(vp._41, vp._42, vp._43, vp._44);
    XMVECTOR p0 = XMPlaneNormalize(XMVectorAdd(r3, r0));      // left
    XMVECTOR p1 = XMPlaneNormalize(XMVectorSubtract(r3, r0)); // right
    XMVECTOR p2 = XMPlaneNormalize(XMVectorAdd(r3, r1));      // bottom
    XMVECTOR p3 = XMPlaneNormalize(XMVectorSubtract(r3, r1)); // top
    XMVECTOR p4 = XMPlaneNormalize(r2);                       // near
    XMVECTOR p5 = XMPlaneNormalize(XMVectorSubtract(r3, r2)); // far

    VertexConstants constants{};
    constants.cameraPos = camera.GetPosition();
    XMStoreFloat4(&constants.planes[0], p0);
    XMStoreFloat4(&constants.planes[1], p1);
    XMStoreFloat4(&constants.planes[2], p2);
    XMStoreFloat4(&constants.planes[3], p3);
    XMStoreFloat4(&constants.planes[4], p4);
    XMStoreFloat4(&constants.planes[5], p5);

    XMMATRIX MprojJ = XMLoadFloat4x4(&projJ);
    XMMATRIX MvpJ = XMMatrixMultiply(Mview, MprojJ);
    XMStoreFloat4x4(&constants.view, Mview);
    XMStoreFloat4x4(&constants.viewProj, MvpJ);
    constants.viewProjNoJ = viewProjNoJ;
    constants.prevViewProjNoJ = prevViewProjNoJ;

    SetResourceBufferData(m_ConstantsBuffer, &constants, sizeof(constants), m_FrameInFlightIdx * sizeof(constants));

    XMFLOAT4X4 invView{};
    {
        XMMATRIX MinvView = XMMatrixInverse(nullptr, Mview);
        XMStoreFloat4x4(&invView, MinvView);
    }
    XMFLOAT4X4 invProjJ{};
    {
        XMMATRIX MinvProj = XMMatrixInverse(nullptr, MprojJ);
        XMStoreFloat4x4(&invProjJ, MinvProj);
    }
    XMFLOAT4X4 invViewProj{};
    {
        XMMATRIX MinvViewProj = XMMatrixInverse(nullptr, MvpJ);
        XMStoreFloat4x4(&invViewProj, MinvViewProj);
    }

    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));
    ComPtr<ID3D12GraphicsCommandList7> cmd = frameData.cmd;

    // Depth Pre-Pass
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cmd->OMSetRenderTargets(0, nullptr, false, &m_DSVsHandle[m_FrameInFlightIdx]);
    cmd->ClearDepthStencilView(m_DSVsHandle[m_FrameInFlightIdx], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
    cmd->RSSetViewports(1, &m_RenderViewport);
    cmd->RSSetScissorRects(1, &m_RenderRect);

    auto DrawPrimitives = [&](const Vector<IEPack::DrawItem>& drawItems) {
        for (const IEPack::DrawItem& drawItem : drawItems)
        {
            const GpuPrim& gp = m_Primitives[drawItem.primIndex];

            XMMATRIX MdrawItemWorld = XMLoadFloat4x4(&drawItem.world);
            XMMATRIX MdrawItemWorldInv = XMMatrixInverse(nullptr, MdrawItemWorld);
            XMFLOAT4X4 drawItemWorldInv;
            XMStoreFloat4x4(&drawItemWorldInv, MdrawItemWorldInv);

            PrimitiveConstants rc{};
            rc.world = drawItem.world;
            rc.worldIT = drawItemWorldInv;
            rc.meshletCount = gp.meshletCount;
            rc.materialIdx = drawItem.materialIndex;
            rc.verticesBufferIndex = gp.vertices->srvIndex;
            rc.meshletsBufferIndex = gp.meshlets->srvIndex;
            rc.meshletVerticesBufferIndex = gp.mlVerts->srvIndex;
            rc.meshletTrianglesBufferIndex = gp.mlTris->srvIndex;
            rc.meshletBoundsBufferIndex = gp.mlBounds->srvIndex;
            rc.materialsBufferIndex = m_MaterialsBuffer->srvIndex;

            cmd->SetGraphicsRoot32BitConstants(0, sizeof(rc) / 4, &rc, 0);
            cmd->DispatchMesh((gp.meshletCount + 31) / 32, 1, 1);
        }
    };

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Pre-Pass - Opaque");
    {
        cmd->SetGraphicsRootSignature(m_DepthPrePassOpaqueRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));
        for (CullMode cm : {CullMode_Back, CullMode_None})
        {
            cmd->SetPipelineState(m_DepthPrePassOpaquePSO[cm].Get());
            DrawPrimitives(m_Draw[AlphaMode_Opaque][cm]);
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Pre-Pass - Alpha-Tested");
    {
        cmd->SetGraphicsRootSignature(m_DepthPrePassAlphaTestRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));
        for (CullMode cm : {CullMode_Back, CullMode_None})
        {
            cmd->SetPipelineState(m_DepthPrePassAlphaTestPSO[cm].Get());
            DrawPrimitives(m_Draw[AlphaMode_Mask][cm]);
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);

    if (g_RTShadows_Enabled)
    {
        Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_GENERIC_READ);

        // Shadows RayTracing
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Shadows Ray-Tracing");
        {
            cmd->SetComputeRootSignature(m_RaytracingGlobalRootSignature.Get());
            cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

            cmd->SetPipelineState1(m_DxrStateObject.Get());

            RtShadowsTraceConstants rayTraceRootConstants{};
            rayTraceRootConstants.invViewProj = invViewProj;
            rayTraceRootConstants.outputTextureIndex = m_RaytracingOutputIndex;
            rayTraceRootConstants.tlasIndex = m_RaytracingTlasIndex;
            rayTraceRootConstants.depthSamplerIndex = m_LinearSamplerIdx;
            rayTraceRootConstants.resolutionType = static_cast<u32>(g_RTShadows_Type); // I should use defines to DXC when compiling but apparently there
                                                                                       // is an DXC issue with -D defines with raytracing shaders...
            rayTraceRootConstants.sunDir = m_SunDir;
            rayTraceRootConstants.frameIndex = m_FrameIndex;
            rayTraceRootConstants.cameraPos = camera.GetPosition();
            rayTraceRootConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
            cmd->SetComputeRoot32BitConstants(0, sizeof(RtShadowsTraceConstants) / sizeof(u32), &rayTraceRootConstants, 0);

            XMUINT2 rtShadowsRes = m_RenderSize;
            switch (g_RTShadows_Type)
            {
            case RayTracingResolution::Full:
                break;
            case RayTracingResolution::FullX_HalfY:
                rtShadowsRes.y /= 2;
                break;
            case RayTracingResolution::Half:
                rtShadowsRes.x /= 2;
                rtShadowsRes.y /= 2;
                break;
            case RayTracingResolution::Quarter:
                rtShadowsRes.x /= 4;
                rtShadowsRes.y /= 4;
                break;
            }

            D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
            dispatchDesc.RayGenerationShaderRecord.StartAddress = m_RayGenShaderTable->GetGPUVirtualAddress();
            dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_RayGenShaderTable->GetDesc().Width;
            dispatchDesc.MissShaderTable.StartAddress = m_MissShaderTable->GetGPUVirtualAddress();
            dispatchDesc.MissShaderTable.SizeInBytes = m_MissShaderTable->GetDesc().Width;
            dispatchDesc.MissShaderTable.StrideInBytes = m_MissShaderTable->GetDesc().Width;
            dispatchDesc.HitGroupTable.StartAddress = m_HitGroupShaderTable->GetGPUVirtualAddress();
            dispatchDesc.HitGroupTable.SizeInBytes = m_HitGroupShaderTable->GetDesc().Width;
            dispatchDesc.HitGroupTable.StrideInBytes = m_HitGroupShaderTable->GetDesc().Width;
            dispatchDesc.Width = rtShadowsRes.x;
            dispatchDesc.Height = rtShadowsRes.y;
            dispatchDesc.Depth = 1;
            cmd->DispatchRays(&dispatchDesc);
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);

        // Blur RT Shadows
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Ray-Traced Shadows Blur");
        {
            RTShadowsBlurConstants rootConstants;
            rootConstants.zNear = zNearFar.x;
            rootConstants.zFar = zNearFar.y;
            rootConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];

            cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
            cmd->SetComputeRootSignature(m_BlurRootSignature.Get());

            // Horizontal pass
            UAVBarrier(cmd, m_RaytracingOutput);
            cmd->SetPipelineState(m_BlurHPso.Get());
            rootConstants.inputTextureIndex = m_SrvRawIdx;
            rootConstants.outputTextureIndex = m_UavTempIdx;
            cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / 4, &rootConstants, 0);
            cmd->Dispatch((m_RenderSize.x + 15) / 16, (m_RenderSize.y + 15) / 16, 1);

            UAVBarrier(cmd, m_BlurTemp);

            // Vertical pass
            cmd->SetPipelineState(m_BlurVPso.Get());
            rootConstants.inputTextureIndex = m_SrvTempIdx;
            rootConstants.outputTextureIndex = m_RaytracingOutputIndex;
            cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / 4, &rootConstants, 0);
            cmd->Dispatch((m_RenderSize.x + 15) / 16, (m_RenderSize.y + 15) / 16, 1);
            UAVBarrier(cmd, m_RaytracingOutput);
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);

        Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_READ);
    }

    GBuffer& gb = m_GBuffers[m_FrameInFlightIdx];

    // Helper to transition all GBuffer targets together
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
        cmd->ClearRenderTargetView(m_GBuffersRTV[m_FrameInFlightIdx][i], clearColor, 0, nullptr);
    }

    for (AlphaMode alphaMode : {AlphaMode_Opaque, AlphaMode_Mask})
    {
        const char* passName = alphaMode == AlphaMode_Opaque ? "GBuffer Opaque Pass" : "GBuffer Masked Pass";
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, passName);
        {
            cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
            cmd->SetGraphicsRootSignature(m_GBufferPassRootSigs[alphaMode].Get());
            cmd->OMSetRenderTargets(GBuffer::targetCount, m_GBuffersRTV[m_FrameInFlightIdx].data(), false, &m_DSVsHandle[m_FrameInFlightIdx]);
            cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));

            // Culled
            cmd->SetPipelineState(m_GBufferPassPSOs[alphaMode][CullMode_Back].Get());
            DrawPrimitives(m_Draw[alphaMode][CullMode_Back]);

            // No-cull
            cmd->SetPipelineState(m_GBufferPassPSOs[alphaMode][CullMode_None].Get());
            DrawPrimitives(m_Draw[alphaMode][CullMode_None]);
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    BarrierGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "SSAO Pass");
    {
        Barrier(cmd, m_SSAOTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_SSAOPso.Get());
        cmd->SetComputeRootSignature(m_SSAORootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

        SSAOConstants ssaoConstants{};
        ssaoConstants.radius = g_SSAO_SampleRadius;
        ssaoConstants.bias = g_SSAO_SampleBias;
        ssaoConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
        ssaoConstants.normalTextureIndex = gb.normalIndex;
        ssaoConstants.proj = projJ;
        ssaoConstants.invProj = invProjJ;
        ssaoConstants.view = view;
        ssaoConstants.renderTargetSize = {static_cast<f32>(m_RenderSize.x), static_cast<f32>(m_RenderSize.y)};
        ssaoConstants.ssaoTextureIndex = m_SSAOUavIdx;
        ssaoConstants.samplerIndex = m_LinearSamplerIdx;
        ssaoConstants.zNear = zNearFar.x;
        ssaoConstants.power = g_SSAO_Power;
        cmd->SetComputeRoot32BitConstants(0, sizeof(SSAOConstants) / 4, &ssaoConstants, 0);

        cmd->Dispatch((m_RenderSize.x + 15) / 16, (m_RenderSize.y + 15) / 16, 1);
        UAVBarrier(cmd, m_SSAOTexture);
        Barrier(cmd, m_SSAOTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    BarrierGBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Lighting Pass");
    {
        LightingPassConstants lightingPassConstants{};
        lightingPassConstants.albedoTextureIndex = m_GBuffers[m_FrameInFlightIdx].albedoIndex;
        lightingPassConstants.normalTextureIndex = m_GBuffers[m_FrameInFlightIdx].normalIndex;
        lightingPassConstants.materialTextureIndex = m_GBuffers[m_FrameInFlightIdx].materialIndex;
        lightingPassConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
        lightingPassConstants.samplerIndex = m_LinearSamplerIdx;
        lightingPassConstants.cameraPos = camera.GetPosition();
        lightingPassConstants.view = view;
        lightingPassConstants.invView = invView;
        lightingPassConstants.invViewProj = invViewProj;
        lightingPassConstants.sunDir = m_SunDir;
        lightingPassConstants.raytracingOutputIndex = m_RaytracingOutputIndex;
        lightingPassConstants.envMapIndex = m_EnvCubeMapSrvIdx;
        lightingPassConstants.diffuseIBLIndex = m_DiffuseIBLIdx;
        lightingPassConstants.specularIBLIndex = m_SpecularIBLIdx;
        lightingPassConstants.brdfLUTIndex = m_BrdfLUTIdx;
        lightingPassConstants.sunAzimuth = g_Sun_Azimuth;
        lightingPassConstants.IBLDiffuseIntensity = g_IBL_DiffuseIntensity;
        lightingPassConstants.IBLSpecularIntensity = g_IBL_SpecularIntensity;
        lightingPassConstants.RTShadowsEnabled = g_RTShadows_Enabled;
        lightingPassConstants.RTShadowsIBLDiffuseStrength = g_RTShadows_IBLDiffuseIntensity;
        lightingPassConstants.RTShadowsIBLSpecularStrength = g_RTShadows_IBLSpecularIntensity;
        lightingPassConstants.renderSize = {static_cast<f32>(m_RenderSize.x), static_cast<f32>(m_RenderSize.y)};
        lightingPassConstants.ssaoTextureIndex = m_SSAOSrvIdx;
        lightingPassConstants.sunIntensity = g_Sun_Intensity;
        lightingPassConstants.skyIntensity = g_IBL_SkyIntensity;
        lightingPassConstants.aoTextureIndex = m_GBuffers[m_FrameInFlightIdx].aoIndex;
        memcpy(m_LightingCBVMapped[m_FrameInFlightIdx], &lightingPassConstants, sizeof(lightingPassConstants));

        cmd->OMSetRenderTargets(1, &m_HDR_RTVsHandle[m_FrameInFlightIdx], false, nullptr);
        cmd->SetPipelineState(m_LightingPassPSO.Get());
        cmd->SetGraphicsRootSignature(m_LightingPassRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetGraphicsRootConstantBufferView(0, m_LightingCBVs[m_FrameInFlightIdx]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    cmd->RSSetViewports(1, &m_PresentViewport);
    cmd->RSSetScissorRects(1, &m_PresentRect);

    auto EnsureFsrState = [&](u32 idx, D3D12_RESOURCE_STATES desired) {
        if (m_FsrOutputState[idx] != desired)
        {
            Barrier(cmd, m_FsrOutputs[idx], m_FsrOutputState[idx], desired);
            m_FsrOutputState[idx] = desired;
        }
    };

    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.motionVector, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    EnsureFsrState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_FrameIndex > 0)
    {
        u32 prevIdx = (m_FrameInFlightIdx + IE_Constants::frameInFlightCount - 1) % IE_Constants::frameInFlightCount;
        EnsureFsrState(prevIdx, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "FSR");
    {
        ffx::DispatchDescUpscale dispatchUpscale{};

        FfxApiResource hdrColor{};
        hdrColor.resource = m_HDR_RTVs[m_FrameInFlightIdx].Get();
        hdrColor.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 0, 0};
        hdrColor.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
        dispatchUpscale.color = hdrColor;

        FfxApiResource depth{};
        depth.resource = m_DSVs[m_FrameInFlightIdx].Get();
        depth.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R32_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 0, 0};
        depth.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        dispatchUpscale.depth = depth;

        FfxApiResource motionVectors{};
        motionVectors.resource = m_GBuffers[m_FrameInFlightIdx].motionVector.Get();
        motionVectors.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 0, 0};
        motionVectors.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        dispatchUpscale.motionVectors = motionVectors;

        FfxApiResource output{};
        output.resource = m_FsrOutputs[m_FrameInFlightIdx].Get();
        output.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, m_PresentSize.x, m_PresentSize.y, 1, 1, 0, 0};
        output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        dispatchUpscale.output = output;

        dispatchUpscale.commandList = cmd.Get();
        dispatchUpscale.jitterOffset.x = jitterNormX;
        dispatchUpscale.jitterOffset.y = jitterNormY;
        dispatchUpscale.cameraFovAngleVertical = IE_ToRadians(g_Camera_FOV);
        dispatchUpscale.cameraNear = zNearFar.y; // Inverted for some reason for FSR
        dispatchUpscale.cameraFar = zNearFar.x;  // Inverted for some reason for FSR
        dispatchUpscale.motionVectorScale.x = static_cast<f32>(m_RenderSize.x);
        dispatchUpscale.motionVectorScale.y = static_cast<f32>(m_RenderSize.y);
        dispatchUpscale.frameTimeDelta = Window::GetFrameTimeMs();
        dispatchUpscale.renderSize.width = m_RenderSize.x;
        dispatchUpscale.renderSize.height = m_RenderSize.y;
        dispatchUpscale.preExposure = 1.0f;

        IE_Assert(ffx::Dispatch(m_UpscalingContext, dispatchUpscale) == ffx::ReturnCode::Ok);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    UAVBarrier(cmd, m_FsrOutputs[m_FrameInFlightIdx]);
    EnsureFsrState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Histogram Pass");
    {
        // Clear
        ClearConstants clearConstants{};
        clearConstants.bufferIndex = m_HistogramBuffer->uavIndex;
        clearConstants.numElements = m_HistogramBuffer->numElements;
        cmd->SetPipelineState(m_ClearUintPso.Get());
        cmd->SetComputeRootSignature(m_ClearUintRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(clearConstants) / 4, &clearConstants, 0);
        cmd->Dispatch((m_HistogramBuffer->numElements + 63) / 64, 1, 1);

        // Histogram
        UAVBarrier(cmd, m_HistogramBuffer->buffer);

        HistogramConstants histogramConstants{};
        histogramConstants.hdrTextureIndex = m_FsrSrvIdx[m_FrameInFlightIdx];
        histogramConstants.minLogLum = g_AutoExposure_MinLogLum;
        histogramConstants.maxLogLum = g_AutoExposure_MaxLogLum;
        histogramConstants.numBuckets = m_HistogramNumBuckets;
        histogramConstants.histogramBufferIndex = m_HistogramBuffer->uavIndex;
        histogramConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
        cmd->SetPipelineState(m_HistogramPso.Get());
        cmd->SetComputeRootSignature(m_HistogramRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(histogramConstants) / 4, &histogramConstants, 0);
        cmd->Dispatch((m_RenderSize.x + 15) / 16, (m_RenderSize.y + 16 - 1) / 16, 1);

        // Exposure
        UAVBarrier(cmd, m_HistogramBuffer->buffer);

        ExposureConstants exposureConstants{};
        exposureConstants.numBuckets = m_HistogramNumBuckets;
        exposureConstants.totalPixels = m_RenderSize.x * m_RenderSize.y;
        exposureConstants.targetPct = g_AutoExposure_TargetPct;
        exposureConstants.lowReject = g_AutoExposure_LowReject;
        exposureConstants.highReject = g_AutoExposure_HighReject;
        exposureConstants.key = g_AutoExposure_Key;
        exposureConstants.minLogLum = g_AutoExposure_MinLogLum;
        exposureConstants.maxLogLum = g_AutoExposure_MaxLogLum;
        exposureConstants.histogramBufferIndex = m_HistogramBuffer->srvIndex;
        exposureConstants.exposureBufferIndex = m_ExposureBuffer->uavIndex;
        cmd->SetPipelineState(m_ExposurePso.Get());
        cmd->SetComputeRootSignature(m_ExposureRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(exposureConstants) / 4, &exposureConstants, 0);
        cmd->Dispatch(1, 1, 1);

        UAVBarrier(cmd, m_ExposureBuffer->buffer);

        AdaptExposureConstants adaptExposureConstants{};
        adaptExposureConstants.exposureBufferIndex = m_ExposureBuffer->srvIndex;
        adaptExposureConstants.adaptedExposureBufferIndex = m_AdaptExposureBuffer->uavIndex;
        adaptExposureConstants.dt = Window::GetFrameTimeMs() / 1000.f;
        adaptExposureConstants.tauBright = g_AutoExposure_TauBright;
        adaptExposureConstants.tauDark = g_AutoExposure_TauDark;
        adaptExposureConstants.clampMin = g_AutoExposure_ClampMin;
        adaptExposureConstants.clampMax = g_AutoExposure_ClampMax;
        cmd->SetPipelineState(m_AdaptExposurePso.Get());
        cmd->SetComputeRootSignature(m_AdaptExposureRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(adaptExposureConstants) / 4, &adaptExposureConstants, 0);
        cmd->Dispatch(1, 1, 1);

        UAVBarrier(cmd, m_AdaptExposureBuffer->buffer);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    // Tone mapping pass
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Tone Mapping");
    {
        EnsureFsrState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TonemapConstants toneMappingRootConstants{};
        toneMappingRootConstants.srvIndex = m_FsrSrvIdx[m_FrameInFlightIdx];
        toneMappingRootConstants.samplerIndex = m_LinearSamplerIdx;
        toneMappingRootConstants.whitePoint = g_ToneMapping_WhitePoint;
        toneMappingRootConstants.contrast = g_ToneMapping_Contrast;
        toneMappingRootConstants.saturation = g_ToneMapping_Saturation;
        toneMappingRootConstants.adaptExposureBufferIndex = m_AdaptExposureBuffer->srvIndex;
        Barrier(cmd, m_RTVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1, &m_RTVsHandle[m_FrameInFlightIdx], false, nullptr);

        cmd->SetPipelineState(m_ToneMapPso.Get());
        cmd->SetGraphicsRootSignature(m_ToneMapRootSignature.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(toneMappingRootConstants) / 4, &toneMappingRootConstants, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    // ImGui
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "ImGui");
    {
        Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, gb.motionVector, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        static ImGui_TimingRaw raw[128];
        static ImGui_TimingSmooth smt[128];

        uint32_t nRaw = m_GpuTimingState.lastCount;
        for (uint32_t i = 0; i < nRaw; ++i)
        {
            raw[i] = {m_GpuTimingState.last[i].name, m_GpuTimingState.last[i].ms};
        }

        uint32_t nSmt = m_GpuTimingState.smoothCount;
        for (uint32_t i = 0; i < nSmt; ++i)
        {
            smt[i] = {m_GpuTimingState.smooth[i].name, m_GpuTimingState.smooth[i].value};
        }

        XMFLOAT3 cameraPos = Camera::GetInstance().GetPosition();

        ImGui_FrameStats frameStats;
        frameStats.fps = Window::GetFPS();
        frameStats.cameraPos[0] = cameraPos.x;
        frameStats.cameraPos[1] = cameraPos.y;
        frameStats.cameraPos[2] = cameraPos.z;

        ImGui_RenderParams rp;
        rp.cmd = cmd.Get();
        rp.rtv = m_RTVsHandle[m_FrameInFlightIdx];
        rp.rtvResource = m_RTVs[m_FrameInFlightIdx].Get();
        rp.gbufferAlbedo = m_GBuffers[m_FrameInFlightIdx].albedo.Get();
        rp.gbufferNormal = m_GBuffers[m_FrameInFlightIdx].normal.Get();
        rp.gbufferMaterial = m_GBuffers[m_FrameInFlightIdx].material.Get();
        rp.gbufferMotion = m_GBuffers[m_FrameInFlightIdx].motionVector.Get();
        rp.gbufferAO = m_GBuffers[m_FrameInFlightIdx].ao.Get();
        rp.depth = m_DSVs[m_FrameInFlightIdx].Get();
        rp.rtShadows = m_RaytracingOutput.Get();
        rp.ssao = m_SSAOTexture.Get();
        rp.renderWidth = m_RenderSize.x;
        rp.renderHeight = m_RenderSize.y;
        rp.frame = frameStats;
        rp.timingsRaw = raw;
        rp.timingsRawCount = nRaw;
        rp.timingsSmooth = smt;
        rp.timingsSmoothCount = nSmt;
        ImGui_Render(rp);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    // Resolve GPU timings
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
        adapter->GetDesc1(&desc);
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
    swapchainDesc.Width = m_PresentSize.x;
    swapchainDesc.Height = m_PresentSize.y;
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
    IE_Check(m_DxgiFactory->MakeWindowAssociation(window.GetHwnd(), DXGI_MWA_NO_ALT_ENTER)); // No fullscreen with alt enter
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
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptorRtvHeapDesc{};
        descriptorRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descriptorRtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
        IE_Check(m_Device->CreateDescriptorHeap(&descriptorRtvHeapDesc, IID_PPV_ARGS(&m_RTVsHeap)));
        IE_Check(m_RTVsHeap->SetName(L"SDR Color Target : Heap"));

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
        {
            IE_Check(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_RTVs[i])));
            IE_Check(m_RTVs[i]->SetName(L"SDR Color Target"));

            m_RTVsHandle[i] = m_RTVsHeap->GetCPUDescriptorHandleForHeapStart();
            m_RTVsHandle[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            m_Device->CreateRenderTargetView(m_RTVs[i].Get(), &rtvDesc, m_RTVsHandle[i]);
        }
    }

    // HDR
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptorRtvHeapDesc{};
        descriptorRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descriptorRtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;

        IE_Check(m_Device->CreateDescriptorHeap(&descriptorRtvHeapDesc, IID_PPV_ARGS(&m_HDR_RTVsHeap)));
        IE_Check(m_HDR_RTVsHeap->SetName(L"HDR Float Color Target : Heap"));

        CD3DX12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
        {
            CD3DX12_RESOURCE_DESC hdrDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            IE_Check(m_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &hdrDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&m_HDR_RTVs[i])));
            IE_Check(m_HDR_RTVs[i]->SetName(L"HDR Float Color Target"));

            D3D12_RENDER_TARGET_VIEW_DESC hdrRtvDesc{};
            hdrRtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            hdrRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_HDR_RTVsHandle[i] = m_HDR_RTVsHeap->GetCPUDescriptorHandleForHeapStart();
            m_HDR_RTVsHandle[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            m_Device->CreateRenderTargetView(m_HDR_RTVs[i].Get(), &hdrRtvDesc, m_HDR_RTVsHandle[i]);
        }
    }
}

void Renderer::CreateDSV()
{
    D3D12_DESCRIPTOR_HEAP_DESC descriptorDsvHeapDesc{};
    descriptorDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    descriptorDsvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorDsvHeapDesc, IID_PPV_ARGS(&m_DSVsHeap)));
    IE_Check(m_DSVsHeap->SetName(L"Depth/Stencil : Heap"));
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 0.0f;

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, m_RenderSize.x, m_RenderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        IE_Check(
            m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, m_DSVsAllocations[i].ReleaseAndGetAddressOf(), IID_PPV_ARGS(&m_DSVs[i])));
        IE_Check(m_DSVs[i]->SetName(L"Depth/Stencil : DSV"));

        m_DSVsHandle[i] = m_DSVsHeap->GetCPUDescriptorHandleForHeapStart();
        m_DSVsHandle[i].ptr += i * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        m_Device->CreateDepthStencilView(m_DSVs[i].Get(), &dsvDesc, m_DSVsHandle[i]);
    }
}

void Renderer::SetRenderAndPresentSize()
{
    m_PresentSize = Window::GetInstance().GetResolution();

    ffx::QueryDescUpscaleGetRenderResolutionFromQualityMode query{};
    query.displayWidth = m_PresentSize.x;
    query.displayHeight = m_PresentSize.y;
    query.qualityMode = IE_Constants::upscalingMode;
    query.pOutRenderWidth = &m_RenderSize.x;
    query.pOutRenderHeight = &m_RenderSize.y;
    IE_Assert(ffx::Query(query) == ffx::ReturnCode::Ok);

    m_PresentViewport.TopLeftX = 0.f;
    m_PresentViewport.TopLeftY = 0.f;
    m_PresentViewport.Width = static_cast<f32>(m_PresentSize.x);
    m_PresentViewport.Height = static_cast<f32>(m_PresentSize.y);
    m_PresentViewport.MinDepth = 0.f;
    m_PresentViewport.MaxDepth = 1.f;

    m_PresentRect.left = 0;
    m_PresentRect.top = 0;
    m_PresentRect.right = static_cast<i32>(m_PresentSize.x);
    m_PresentRect.bottom = static_cast<i32>(m_PresentSize.y);

    m_RenderViewport.TopLeftX = 0.f;
    m_RenderViewport.TopLeftY = 0.f;
    m_RenderViewport.Width = static_cast<f32>(m_RenderSize.x);
    m_RenderViewport.Height = static_cast<f32>(m_RenderSize.y);
    m_RenderViewport.MinDepth = 0.f;
    m_RenderViewport.MaxDepth = 1.f;

    m_RenderRect.left = 0;
    m_RenderRect.top = 0;
    m_RenderRect.right = static_cast<i32>(m_RenderSize.x);
    m_RenderRect.bottom = static_cast<i32>(m_RenderSize.y);
}

void Renderer::CreateGPUTimers()
{
    IE_Check(m_CommandQueue->GetTimestampFrequency(&m_GpuTimingState.timestampFrequency));
    u32 kMaxTimestamps = 256;

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kMaxTimestamps;
        IE_Check(m_Device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_AllFrameData[i].gpuTimers.heap)));

        CD3DX12_RESOURCE_DESC rb = CD3DX12_RESOURCE_DESC::Buffer(kMaxTimestamps * sizeof(u64));
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
    createUpscaling.maxRenderSize = {m_RenderSize.x, m_RenderSize.y};
    createUpscaling.maxUpscaleSize = {m_PresentSize.x, m_PresentSize.y};
    createUpscaling.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE; //| FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    IE_Assert(ffx::CreateContext(m_UpscalingContext, nullptr, createUpscaling, backendDesc) == ffx::ReturnCode::Ok);

    ffx::QueryDescUpscaleGetJitterPhaseCount getJitterPhaseDesc{};
    getJitterPhaseDesc.displayWidth = m_PresentSize.x;
    getJitterPhaseDesc.renderWidth = m_RenderSize.x;
    getJitterPhaseDesc.pOutPhaseCount = &m_JitterPhaseCount;

    IE_Assert(ffx::Query(m_UpscalingContext, getJitterPhaseDesc) == ffx::ReturnCode::Ok);

    ffx::QueryDescUpscaleGetJitterOffset getJitterOffsetDesc{};
    getJitterOffsetDesc.index = m_JitterIndex;
    getJitterOffsetDesc.phaseCount = m_JitterPhaseCount;
    getJitterOffsetDesc.pOutX = &m_JitterX;
    getJitterOffsetDesc.pOutY = &m_JitterY;

    IE_Assert(ffx::Query(m_UpscalingContext, getJitterOffsetDesc) == ffx::ReturnCode::Ok);

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        CD3DX12_RESOURCE_DESC fsrDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_PresentSize.x, m_PresentSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        IE_Check(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &fsrDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_FsrOutputs[i])));
        IE_Check(m_FsrOutputs[i]->SetName(L"FSR Output"));

        D3D12_SHADER_RESOURCE_VIEW_DESC fsrSrvDesc{};
        fsrSrvDesc.Format = fsrDesc.Format;
        fsrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        fsrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        fsrSrvDesc.Texture2D.MipLevels = 1;
        m_FsrSrvIdx[i] = m_BindlessHeaps.CreateSRV(m_FsrOutputs[i].Get(), fsrSrvDesc);
    }
}

void Renderer::CreateDepthPrePassResources(const Vector<WString>& globalDefines)
{
    m_AmplificationShader = LoadShader(IE_SHADER_TYPE_AMPLIFICATION, L"asBasic.hlsl", {globalDefines});
    m_MeshShader = LoadShader(IE_SHADER_TYPE_MESH, L"msBasic.hlsl", {globalDefines});

    CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
    ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    // Opaque
    {
        IE_Check(m_Device->CreateRootSignature(0, m_MeshShader.bytecode.pShaderBytecode, m_MeshShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_DepthPrePassOpaqueRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPrePassOpaqueRootSig.Get();
        depthDesc.AS = m_AmplificationShader.bytecode.BytecodeLength > 0 ? m_AmplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        depthDesc.MS = m_MeshShader.bytecode;
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPrePassOpaquePSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPrePassOpaquePSO[CullMode_None])));
    }

    // Alpha Tested
    {
        m_DepthPrePassAlphaTestShader = LoadShader(IE_SHADER_TYPE_PIXEL, L"psAlphaTest.hlsl", {globalDefines});

        IE_Check(m_Device->CreateRootSignature(0, m_DepthPrePassAlphaTestShader.bytecode.pShaderBytecode, m_DepthPrePassAlphaTestShader.bytecode.BytecodeLength,
                                               IID_PPV_ARGS(&m_DepthPrePassAlphaTestRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPrePassAlphaTestRootSig.Get();
        depthDesc.AS = m_AmplificationShader.bytecode.BytecodeLength > 0 ? m_AmplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        depthDesc.MS = m_MeshShader.bytecode;
        depthDesc.PS = m_DepthPrePassAlphaTestShader.bytecode;
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPrePassAlphaTestPSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPrePassAlphaTestPSO[CullMode_None])));
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

    m_PixelShader[AlphaMode_Opaque] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", globalDefines);

    Vector<WString> blendDefines = globalDefines;
    blendDefines.push_back(L"ENABLE_BLEND");
    m_PixelShader[AlphaMode_Blend] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", blendDefines);

    Vector<WString> maskDefines = globalDefines;
    maskDefines.push_back(L"ENABLE_ALPHA_TEST");
    m_PixelShader[AlphaMode_Mask] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", maskDefines);

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        GBuffer& gbuff = m_GBuffers[i];
        ComPtr<ID3D12Resource>* targets[GBuffer::targetCount] = {&gbuff.albedo, &gbuff.normal, &gbuff.material, &gbuff.motionVector, &gbuff.ao};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = GBuffer::targetCount;
        IE_Check(m_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gbuff.rtvHeap)));

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gbuff.rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (u32 t = 0; t < GBuffer::targetCount; ++t)
        {
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[t], m_RenderSize.x, m_RenderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE clearValue = {formats[t], {0.f, 0.f, 0.f, 0.f}};
            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

            IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(targets[t]->GetAddressOf())));
            IE_Check(targets[t]->Get()->SetName(rtvNames[t]));

            m_Device->CreateRenderTargetView(targets[t]->Get(), nullptr, rtvHandle);
            m_GBuffersRTV[i][t] = rtvHandle;

            rtvHandle.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }

    for (AlphaMode alphaMode = AlphaMode_Opaque; alphaMode < AlphaMode_Count; alphaMode = static_cast<AlphaMode>(alphaMode + 1))
    {
        IE_Check(m_Device->CreateRootSignature(0, m_MeshShader.bytecode.pShaderBytecode, m_MeshShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_GBufferPassRootSigs[alphaMode])));

        CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
        ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC msDesc{};
        msDesc.pRootSignature = m_GBufferPassRootSigs[alphaMode].Get();
        msDesc.AS = m_AmplificationShader.bytecode.BytecodeLength > 0 ? m_AmplificationShader.bytecode : D3D12_SHADER_BYTECODE{};
        msDesc.MS = m_MeshShader.bytecode;
        msDesc.PS = m_PixelShader[alphaMode].bytecode;
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

        // cull
        {
            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(msDesc);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBufferPassPSOs[alphaMode][CullMode_Back])));
        }

        // no cull
        {
            D3DX12_MESH_SHADER_PIPELINE_STATE_DESC noCull = msDesc;
            D3D12_RASTERIZER_DESC rast = noCull.RasterizerState;
            rast.CullMode = D3D12_CULL_MODE_NONE;
            noCull.RasterizerState = rast;

            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(noCull);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBufferPassPSOs[alphaMode][CullMode_None])));
        }
    }
}

void Renderer::CreateLightingPassResources(const Vector<WString>& globalDefines)
{
    m_LightingPassShader = LoadShader(IE_SHADER_TYPE_PIXEL, L"psLighting.hlsl", {globalDefines});
    ComPtr<IDxcBlob> vsFullscreen = CompileShader(IE_SHADER_TYPE_VERTEX, L"vsFullscreen.hlsl", {});

    // Root Signature
    IE_Check(m_Device->CreateRootSignature(0, m_LightingPassShader.bytecode.pShaderBytecode, m_LightingPassShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_LightingPassRootSig)));

    // PSO
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_LightingPassRootSig.Get();
    psoDesc.VS.pShaderBytecode = vsFullscreen->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vsFullscreen->GetBufferSize();
    psoDesc.PS.pShaderBytecode = m_LightingPassShader.bytecode.pShaderBytecode;
    psoDesc.PS.BytecodeLength = m_LightingPassShader.bytecode.BytecodeLength;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    psoDesc.SampleDesc = DefaultSampleDesc();
    IE_Check(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_LightingPassPSO)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv2D{};
    srv2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv2D.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv2D.Texture2D.MipLevels = 1;

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(LightingPassConstants) + 255 & ~255);

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        srv2D.Format = m_GBuffers[i].albedo->GetDesc().Format;
        m_GBuffers[i].albedoIndex = m_BindlessHeaps.CreateSRV(m_GBuffers[i].albedo, srv2D);

        srv2D.Format = m_GBuffers[i].normal->GetDesc().Format;
        m_GBuffers[i].normalIndex = m_BindlessHeaps.CreateSRV(m_GBuffers[i].normal, srv2D);

        srv2D.Format = m_GBuffers[i].material->GetDesc().Format;
        m_GBuffers[i].materialIndex = m_BindlessHeaps.CreateSRV(m_GBuffers[i].material, srv2D);

        srv2D.Format = m_GBuffers[i].motionVector->GetDesc().Format;
        m_GBuffers[i].motionVectorIndex = m_BindlessHeaps.CreateSRV(m_GBuffers[i].motionVector, srv2D);

        srv2D.Format = m_GBuffers[i].ao->GetDesc().Format;
        m_GBuffers[i].aoIndex = m_BindlessHeaps.CreateSRV(m_GBuffers[i].ao, srv2D);

        IE_Check(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_LightingCBVs[i])));
        IE_Check(m_LightingCBVs[i]->SetName(L"LightingPassConstants"));
        m_LightingCBVs[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_LightingCBVMapped[i]));
    }
}

void Renderer::CreateHistogramPassResources(const Vector<WString>& globalDefines)
{
    m_HistogramBuffer = CreateStructuredBuffer(m_HistogramNumBuckets * sizeof(u32), sizeof(u32), L"Histogram");

    // Clear pass
    m_ClearUintShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csClearUint.hlsl", {});

    IE_Check(m_Device->CreateRootSignature(0, m_ClearUintShader.bytecode.pShaderBytecode, m_ClearUintShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_ClearUintRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC clearUintPsoDesc{};
    clearUintPsoDesc.pRootSignature = m_ClearUintRootSig.Get();
    clearUintPsoDesc.CS.pShaderBytecode = m_ClearUintShader.bytecode.pShaderBytecode;
    clearUintPsoDesc.CS.BytecodeLength = m_ClearUintShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&clearUintPsoDesc, IID_PPV_ARGS(&m_ClearUintPso)));

    // Histogram pass
    m_HistogramShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csHistogram.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_HistogramShader.bytecode.pShaderBytecode, m_HistogramShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_HistogramRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC histogramPsoDesc{};
    histogramPsoDesc.pRootSignature = m_HistogramRootSig.Get();
    histogramPsoDesc.CS.pShaderBytecode = m_HistogramShader.bytecode.pShaderBytecode;
    histogramPsoDesc.CS.BytecodeLength = m_HistogramShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&histogramPsoDesc, IID_PPV_ARGS(&m_HistogramPso)));

    // Exposure pass
    m_ExposureBuffer = CreateStructuredBuffer(1 * sizeof(f32), sizeof(f32), L"Exposure");
    m_ExposureShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csExposure.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_ExposureShader.bytecode.pShaderBytecode, m_ExposureShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_ExposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC exposurePsoDesc{};
    exposurePsoDesc.pRootSignature = m_ExposureRootSig.Get();
    exposurePsoDesc.CS.pShaderBytecode = m_ExposureShader.bytecode.pShaderBytecode;
    exposurePsoDesc.CS.BytecodeLength = m_ExposureShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&exposurePsoDesc, IID_PPV_ARGS(&m_ExposurePso)));

    // Adapt exposure pass
    m_AdaptExposureBuffer = CreateStructuredBuffer(1 * sizeof(f32), sizeof(f32), L"Adapt Exposure");
    m_AdaptExposureShader = LoadShader(IE_SHADER_TYPE_COMPUTE, L"csAdaptExposure.hlsl", {});
    IE_Check(m_Device->CreateRootSignature(0, m_AdaptExposureShader.bytecode.pShaderBytecode, m_AdaptExposureShader.bytecode.BytecodeLength, IID_PPV_ARGS(&m_AdaptExposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC adaptExposurePsoDesc{};
    adaptExposurePsoDesc.pRootSignature = m_AdaptExposureRootSig.Get();
    adaptExposurePsoDesc.CS.pShaderBytecode = m_AdaptExposureShader.bytecode.pShaderBytecode;
    adaptExposurePsoDesc.CS.BytecodeLength = m_AdaptExposureShader.bytecode.BytecodeLength;
    IE_Check(m_Device->CreateComputePipelineState(&adaptExposurePsoDesc, IID_PPV_ARGS(&m_AdaptExposurePso)));
}

void Renderer::CreateToneMapPassResources(const Vector<WString>& globalDefines)
{
    // Root Signature
    ComPtr<IDxcBlob> vsFullscreen = CompileShader(IE_SHADER_TYPE_VERTEX, L"vsFullscreen.hlsl", {globalDefines});
    ComPtr<IDxcBlob> psTonemap = CompileShader(IE_SHADER_TYPE_PIXEL, L"psTonemap.hlsl", {globalDefines});
    CD3DX12_SHADER_BYTECODE psTonemapBytecode = CD3DX12_SHADER_BYTECODE(psTonemap->GetBufferPointer(), psTonemap->GetBufferSize());
    IE_Check(m_Device->CreateRootSignature(0, psTonemapBytecode.pShaderBytecode, psTonemapBytecode.BytecodeLength, IID_PPV_ARGS(&m_ToneMapRootSignature)));

    // PSO
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_ToneMapRootSignature.Get();
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
    IE_Check(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_ToneMapPso)));
}

void Renderer::CreateSSAOResources(const Vector<WString>& globalDefines)
{
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, m_RenderSize.x, m_RenderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_SSAOTexture)));
    IE_Check(m_SSAOTexture->SetName(L"SSAO Texture"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_SSAOUavIdx = m_BindlessHeaps.CreateUAV(m_SSAOTexture.Get(), uavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_SSAOSrvIdx = m_BindlessHeaps.CreateSRV(m_SSAOTexture.Get(), srvDesc);

    ComPtr<IDxcBlob> cs = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csSSAO.hlsl", {});

    IE_Check(m_Device->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), IID_PPV_ARGS(&m_SSAORootSig)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_SSAORootSig.Get();
    psoDesc.CS.pShaderBytecode = cs->GetBufferPointer();
    psoDesc.CS.BytecodeLength = cs->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_SSAOPso)));
}

void Renderer::LoadScene()
{
    PerFrameData& frameData = GetCurrentFrameData();
    auto& sl = SceneLoader::Get();

    ComPtr<ID3D12GraphicsCommandList7> cmd;
    IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameData.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));

    // --- Scene path & camera ---
    const CommandLineArguments& args = GetCommandLineArguments();
    String sceneFile = args.sceneFile.empty() ? "San-Miguel" : args.sceneFile;
    String scenePath = String("data/scenes/") + sceneFile + ".glb";

    Camera& camera = Camera::GetInstance();
    camera.LoadSceneConfig(sceneFile);

    {
        namespace fs = std::filesystem;
        fs::path fsScenePath = fs::path(scenePath.data());
        fs::path baseDir = fsScenePath.parent_path();
        fs::path packPath = baseDir / (fsScenePath.stem().wstring() + L".iskurpack");
        sl.Open(packPath);
    }

    // --- Textures ---
    {
        const auto& texTable = sl.GetTextureTable();
        const u8* texBlob = sl.GetTextureBlobData();
        const size_t texBlobSize = sl.GetTextureBlobSize();
        const u32 texCount = texTable.size();

        m_TxhdToSrv.resize(texCount);
        m_Textures.resize(texCount); // avoid reallocation thrash

        ResourceUploadBatch batch(m_Device.Get());
        batch.Begin();

        for (u32 i = 0; i < texCount; ++i)
        {
            const auto& tr = texTable[i];
            IE_Assert(tr.byteOffset + tr.byteSize <= texBlobSize);

            const uint8_t* ddsPtr = texBlob + static_cast<size_t>(tr.byteOffset);

            ComPtr<ID3D12Resource> res;
            IE_Check(CreateDDSTextureFromMemory(m_Device.Get(), batch, ddsPtr, tr.byteSize, &res, false, 0, nullptr, nullptr));

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = res->GetDesc().Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = UINT32_MAX;

            const u32 srvIdx = m_BindlessHeaps.CreateSRV(res, srv);
            m_TxhdToSrv[i] = srvIdx;
            m_Textures[i] = res; // write into pre-sized array
        }

        batch.End(m_CommandQueue.Get()).wait();
    }

    // --- Samplers ---
    {
        const auto& sampTable = sl.GetSamplerTable();
        const u32 sampCount = sampTable.size();
        m_SampToHeap.resize(sampCount);

        for (u32 i = 0; i < sampCount; ++i)
        {
            const auto& s = sampTable[i];

            D3D12_SAMPLER_DESC sd{};
            sd.Filter = static_cast<D3D12_FILTER>(s.d3d12Filter);
            sd.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressU);
            sd.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressV);
            sd.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressW);
            sd.MipLODBias = s.mipLODBias;
            sd.MinLOD = s.minLOD;
            sd.MaxLOD = s.maxLOD;
            sd.MaxAnisotropy = static_cast<UINT>(s.maxAnisotropy);
            sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(s.comparisonFunc);
            sd.BorderColor[0] = s.borderColor[0];
            sd.BorderColor[1] = s.borderColor[1];
            sd.BorderColor[2] = s.borderColor[2];
            sd.BorderColor[3] = s.borderColor[3];

            m_SampToHeap[i] = m_BindlessHeaps.CreateSampler(sd);
        }
    }

    // --- Materials ---
    {
        const auto& matlTable = sl.GetMaterialTable();
        const u32 matCount = matlTable.size();

        m_Materials.resize(matCount);

        const auto& txToSrv = m_TxhdToSrv;
        const auto& spToSmpl = m_SampToHeap;

        for (u32 i = 0; i < matCount; ++i)
        {
            const auto& mr = matlTable[i];
            Material m{};
            m.baseColorFactor = {mr.baseColorFactor[0], mr.baseColorFactor[1], mr.baseColorFactor[2], mr.baseColorFactor[3]};
            m.metallicFactor = mr.metallicFactor;
            m.roughnessFactor = mr.roughnessFactor;
            m.normalScale = mr.normalScale;
            m.alphaCutoff = mr.alphaCutoff;
            m.alphaMode = AlphaMode_Opaque;
            if (mr.flags & IEPack::MATF_ALPHA_BLEND)
            {
                m.alphaMode = AlphaMode_Mask; // BLEND NOT SUPPORTED!
            }
            else if (mr.flags & IEPack::MATF_ALPHA_MASK)
            {
                m.alphaMode = AlphaMode_Mask;
            }
            m.doubleSided = !!(mr.flags & IEPack::MATF_DOUBLE_SIDED);

            auto mapTex = [&](int txhdIdx) -> i32 {
                if (txhdIdx < 0)
                    return -1;
                IE_Assert(static_cast<size_t>(txhdIdx) < txToSrv.size());
                return static_cast<i32>(txToSrv[static_cast<u32>(txhdIdx)]);
            };
            auto mapSamp = [&](u32 sampIdx, int txhdIdx) -> i32 {
                if (txhdIdx < 0 || sampIdx == UINT32_MAX)
                    return -1;
                IE_Assert(static_cast<size_t>(sampIdx) < spToSmpl.size());
                return static_cast<i32>(spToSmpl[static_cast<u32>(sampIdx)]);
            };

            m.baseColorTextureIndex = mapTex(mr.baseColorTx);
            m.baseColorSamplerIndex = mapSamp(mr.baseColorSampler, mr.baseColorTx);
            m.metallicRoughnessTextureIndex = mapTex(mr.metallicRoughTx);
            m.metallicRoughnessSamplerIndex = mapSamp(mr.metallicRoughSampler, mr.metallicRoughTx);
            m.normalTextureIndex = mapTex(mr.normalTx);
            m.normalSamplerIndex = mapSamp(mr.normalSampler, mr.normalTx);
            m.aoTextureIndex = mapTex(mr.occlusionTx);
            m.aoSamplerIndex = mapSamp(mr.occlusionSampler, mr.occlusionTx);
            // todo add emissive
            m_Materials[i] = m;
        }

        if (!m_Materials.empty())
        {
            m_MaterialsBuffer = CreateStructuredBuffer(m_Materials.size() * sizeof(Material), sizeof(Material), L"Materials");
            SetBufferData(cmd, m_MaterialsBuffer, m_Materials.data(), m_Materials.size() * sizeof(Material), 0);
        }
    }

    // --- Upload primitives to GPU ---
    {
        // Map packed-prim id -> local GPU prim id
        std::unordered_map<u32, u32> packPrimToLocalId;
        packPrimToLocalId.reserve(1024);

        auto getOrCreatePrim = [&](u32 packedPrimId) -> u32 {
            auto it = packPrimToLocalId.find(packedPrimId);
            if (it != packPrimToLocalId.end())
                return it->second;

            const PackedPrimitiveView* pv = sl.GetPrimitiveById(packedPrimId);
            IE_Assert(pv && "Pack missing primitive referenced by draw list");

            GpuPrim gp{};
            gp.materialIdx = pv->materialIndex;

            // Cast views from the pack
            const Vertex* vtx = reinterpret_cast<const Vertex*>(pv->vertices);
            const u32* idx = reinterpret_cast<const u32*>(pv->indices);
            const Meshlet* mlt = reinterpret_cast<const Meshlet*>(pv->meshlets);
            const u32* mlv = reinterpret_cast<const u32*>(pv->mlVerts);
            const u8* mltb = reinterpret_cast<const u8*>(pv->mlTris);
            const meshopt_Bounds* mlb = reinterpret_cast<const meshopt_Bounds*>(pv->mlBounds);

            gp.meshletCount = pv->meshletCount;

            gp.vertices = CreateStructuredBuffer(pv->vertexCount * sizeof(Vertex), sizeof(Vertex), L"Pack/Vertices");
            SetBufferData(cmd, gp.vertices, vtx, pv->vertexCount * sizeof(Vertex), 0);

            gp.meshlets = CreateBytesBuffer(pv->meshletCount * sizeof(Meshlet), L"Pack/Meshlets");
            SetBufferData(cmd, gp.meshlets, mlt, pv->meshletCount * sizeof(Meshlet), 0);

            gp.mlVerts = CreateStructuredBuffer(pv->mlVertCount * sizeof(u32), sizeof(u32), L"Pack/MeshletVerts");
            SetBufferData(cmd, gp.mlVerts, mlv, pv->mlVertCount * sizeof(u32), 0);

            gp.mlTris = CreateBytesBuffer(pv->mlTriCountBytes, L"Pack/MeshletTris");
            SetBufferData(cmd, gp.mlTris, mltb, pv->mlTriCountBytes, 0);

            gp.mlBounds = CreateStructuredBuffer(pv->mlBoundsCount * sizeof(meshopt_Bounds), sizeof(meshopt_Bounds), L"Pack/MeshletBounds");
            SetBufferData(cmd, gp.mlBounds, mlb, pv->mlBoundsCount * sizeof(meshopt_Bounds), 0);

            // Keep CPU data for RT BLAS build
            gp.cpuVertices = vtx;
            gp.vertexCount = pv->vertexCount;
            gp.cpuIndices = idx;
            gp.indexCount = pv->indexCount;

            const u32 localPrimId = m_Primitives.size();
            m_Primitives.push_back(gp);
            packPrimToLocalId.emplace(packedPrimId, localPrimId);
            return localPrimId;
        };

        auto emitBucket = [&](const Vector<IEPack::DrawItem>& items, const Vector<u32>& instIDs, AlphaMode alphaMode, u32 dsIndex /*0=culled, 1=no-cull*/) {
            const auto& allInsts = sl.GetInstances();

            for (const IEPack::DrawItem& di : items)
            {
                const u32 localPrimId = getOrCreatePrim(di.primIndex);

                IE_Assert(di.instanceBegin + di.instanceCount <= instIDs.size());
                const u32 start = di.instanceBegin;
                const u32 end = di.instanceBegin + di.instanceCount;

                for (u32 k = start; k < end; ++k)
                {
                    const u32 instId = instIDs[k];
                    IE_Assert(instId < allInsts.size());
                    const IEPack::InstanceRecord& inst = allInsts[instId];

                    const u32 matIdx = (inst.materialOverride != UINT32_MAX) ? inst.materialOverride : di.materialIndex;
                    IE_Assert(matIdx < m_Materials.size());
                    const Material& mat = m_Materials[matIdx];

                    IEPack::DrawItem out{};
                    out.primIndex = localPrimId;
                    out.materialIndex = matIdx;
                    out.world = inst.world;
                    out.doubleSided = mat.doubleSided;
                    out.alphaMode = alphaMode;

                    m_Draw[alphaMode][dsIndex].push_back(out);
                }
            }
        };

        // Culled + No-cull buckets
        emitBucket(sl.GetDrawItemsCulledOpaque(), sl.GetDrawInstIDsCulledOpaque(), AlphaMode_Opaque, 0);
        emitBucket(sl.GetDrawItemsCulledMasked(), sl.GetDrawInstIDsCulledMasked(), AlphaMode_Mask, 0);
        emitBucket(sl.GetDrawItemsCulledBlended(), sl.GetDrawInstIDsCulledBlended(), AlphaMode_Blend, 0);
        emitBucket(sl.GetDrawItemsNoCullOpaque(), sl.GetDrawInstIDsNoCullOpaque(), AlphaMode_Opaque, 1);
        emitBucket(sl.GetDrawItemsNoCullMasked(), sl.GetDrawInstIDsNoCullMasked(), AlphaMode_Mask, 1);
        emitBucket(sl.GetDrawItemsNoCullBlended(), sl.GetDrawInstIDsNoCullBlended(), AlphaMode_Blend, 1);
    }

    // --- Depth SRVs and linear sampler ---
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
            m_DSVsIdx[i] = m_BindlessHeaps.CreateSRV(m_DSVs[i], srvDesc);

        D3D12_SAMPLER_DESC linearClampDesc{};
        linearClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linearClampDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampDesc.MaxAnisotropy = 1;
        linearClampDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        linearClampDesc.MaxLOD = D3D12_FLOAT32_MAX;
        m_LinearSamplerIdx = m_BindlessHeaps.CreateSampler(linearClampDesc);
    }

    SetupRaytracing(cmd);

    // --- Env maps ---
    {
        WString envName = L"kloofendal_48d_partly_cloudy_puresky";
        WString basePath = L"data/textures/" + envName;

        ResourceUploadBatch batch(m_Device.Get());
        batch.Begin();

        // Env cube
        IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/envMap.dds").data(), &m_EnvCubeMap, false, 0, nullptr, nullptr));
        IE_Check(m_EnvCubeMap->SetName(L"EnvCubeMap"));

        // Diffuse IBL
        IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/diffuseIBL.dds").data(), &m_DiffuseIBL, false, 0, nullptr, nullptr));
        IE_Check(m_DiffuseIBL->SetName(L"DiffuseIBL"));

        // Specular IBL
        IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, (basePath + L"/specularIBL.dds").data(), &m_SpecularIBL, false, 0, nullptr, nullptr));
        IE_Check(m_SpecularIBL->SetName(L"SpecularIBL"));

        // BRDF LUT (2D)
        IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, L"data/textures/BRDF_LUT.dds", &m_BrdfLUT, false, 0, nullptr, nullptr));
        IE_Check(m_BrdfLUT->SetName(L"BrdfLut"));

        batch.End(m_CommandQueue.Get()).wait();

        // Create SRVs after upload is enqueued
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            // Env cube
            srv.Format = DXGI_FORMAT_BC6H_UF16;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = UINT32_MAX;
            m_EnvCubeMapSrvIdx = m_BindlessHeaps.CreateSRV(m_EnvCubeMap, srv);

            // Diffuse IBL
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = UINT32_MAX;
            m_DiffuseIBLIdx = m_BindlessHeaps.CreateSRV(m_DiffuseIBL, srv);

            // Specular IBL
            srv.Format = DXGI_FORMAT_BC6H_UF16;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = UINT32_MAX;
            m_SpecularIBLIdx = m_BindlessHeaps.CreateSRV(m_SpecularIBL, srv);

            // BRDF LUT (2D)
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = UINT32_MAX;
            m_BrdfLUTIdx = m_BindlessHeaps.CreateSRV(m_BrdfLUT, srv);
        }
    }

    IE_Check(cmd->Close());
    ID3D12CommandList* pCmd = cmd.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmd);

    u64 fenceToWait = ++frameData.frameFenceValue;
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), fenceToWait));

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    IE_Check(frameData.frameFence->SetEventOnCompletion(fenceToWait, evt));
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);

    cmd.Reset();
    m_InFlightUploads.clear();
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

void Renderer::SetupRaytracing(ComPtr<ID3D12GraphicsCommandList7>& cmdList)
{
    // Create 2D output texture for raytracing.
    Shader raytracingShader = LoadShader(IE_SHADER_TYPE_LIB, L"rtShadows.hlsl", {});

    // Create the output resource
    {
        CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CD3DX12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        IE_Check(m_Device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_RaytracingOutput)));
        m_RaytracingOutputIndex = m_BindlessHeaps.CreateUAV(m_RaytracingOutput, uavDesc);
        IE_Check(m_RaytracingOutput->SetName(L"Raytracing Output"));
    }

    CD3DX12_ROOT_PARAMETER rootParameter;
    rootParameter.InitAsConstants(sizeof(RtShadowsTraceConstants) / sizeof(u32), 0);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(1, &rootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSignatureDesc(rsDesc);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    IE_Check(D3D12SerializeVersionedRootSignature(&globalRootSignatureDesc, &blob, &error));
    IE_Check(m_Device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RaytracingGlobalRootSignature)));

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

    D3D12_SHADER_BYTECODE libdxil{raytracingShader.bytecode.pShaderBytecode, raytracingShader.bytecode.BytecodeLength};
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"Miss");

    CD3DX12_HIT_GROUP_SUBOBJECT* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(L"ClosestHit");
    hitGroup->SetHitGroupExport(L"HitGroup");
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* shaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    u32 payloadSize = 1 * sizeof(u32);   // struct RayPayload is just one uint
    u32 attributeSize = 2 * sizeof(f32); // BuiltInTriangleIntersectionAttributes
    shaderConfig->Config(payloadSize, attributeSize);

    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_RaytracingGlobalRootSignature.Get());

    CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    constexpr u32 maxRecursionDepth = 1;
    pipelineConfig->Config(maxRecursionDepth);

    IE_Check(m_Device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_DxrStateObject)));

    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    for (GpuPrim& gp : m_Primitives)
    {
        // Upload VB/IB (RT needs a conventional triangle list)
        AllocateUploadBuffer(gp.cpuVertices, gp.vertexCount * sizeof(Vertex), 0, gp.rtVB, gp.rtVBAlloc, L"PackRT/VB");
        AllocateUploadBuffer(gp.cpuIndices, gp.indexCount * sizeof(u32), 0, gp.rtIB, gp.rtIBAlloc, L"PackRT/IB");

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.Transform3x4 = 0;
        geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.IndexCount = gp.indexCount;
        geom.Triangles.VertexCount = gp.vertexCount;
        geom.Triangles.IndexBuffer = gp.rtIB->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StartAddress = gp.rtVB->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

        AllocateUAVBuffer((u32)info.ResultDataMaxSizeInBytes, gp.blas, gp.blasAlloc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS");
        AllocateUAVBuffer((u32)info.ScratchDataSizeInBytes, gp.scratch, gp.scratchAlloc, D3D12_RESOURCE_STATE_COMMON, L"BLAS Scratch");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData = gp.blas->GetGPUVirtualAddress();
        build.Inputs = in;
        build.ScratchAccelerationStructureData = gp.scratch->GetGPUVirtualAddress();
        cmdList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    }

    for (GpuPrim& gp : m_Primitives)
    {
        UAVBarrier(cmdList, gp.blas);
    }

    for (const auto& bucket : {std::ref(m_Draw[AlphaMode_Opaque][0]), std::ref(m_Draw[AlphaMode_Opaque][1]), std::ref(m_Draw[AlphaMode_Mask][0]), std::ref(m_Draw[AlphaMode_Mask][1])})
    {
        for (const IEPack::DrawItem& drawItem : bucket.get())
        {
            const GpuPrim& gp = m_Primitives[drawItem.primIndex];

            D3D12_RAYTRACING_INSTANCE_DESC id{};
            id.InstanceMask = 1;
            id.AccelerationStructure = gp.blas->GetGPUVirtualAddress();
            const XMFLOAT4X4& W = drawItem.world;

            id.Transform[0][0] = W._11; // m00
            id.Transform[0][1] = W._21; // m01
            id.Transform[0][2] = W._31; // m02
            id.Transform[0][3] = W._41; // m03

            id.Transform[1][0] = W._12; // m10
            id.Transform[1][1] = W._22; // m11
            id.Transform[1][2] = W._32; // m12
            id.Transform[1][3] = W._42; // m13

            id.Transform[2][0] = W._13; // m20
            id.Transform[2][1] = W._23; // m21
            id.Transform[2][2] = W._33; // m22
            id.Transform[2][3] = W._43; // m23

            instanceDescs.push_back(id);
        }
    }

    AllocateUploadBuffer(instanceDescs.data(), instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), 0, m_InstanceDescs, m_InstanceDescsAlloc, L"InstanceDescs");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    topLevelInputs.NumDescs = instanceDescs.size();
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.InstanceDescs = m_InstanceDescs->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo;
    m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
    IE_Assert(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    AllocateUAVBuffer(static_cast<u32>(topLevelPrebuildInfo.ScratchDataSizeInBytes), m_ScratchResource, m_ScratchResourceAlloc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");
    AllocateUAVBuffer(static_cast<u32>(topLevelPrebuildInfo.ResultDataMaxSizeInBytes), m_TLAS, m_TLASAlloc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc{};
    topLevelBuildDesc.DestAccelerationStructureData = m_TLAS->GetGPUVirtualAddress();
    topLevelBuildDesc.Inputs = topLevelInputs;
    topLevelBuildDesc.ScratchAccelerationStructureData = m_ScratchResource->GetGPUVirtualAddress();
    cmdList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = m_TLAS->GetGPUVirtualAddress();
    m_RaytracingTlasIndex = m_BindlessHeaps.CreateSRV(nullptr, srvDesc);

    ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    IE_Check(m_DxrStateObject.As(&stateObjectProperties));

    u32 shaderRecordSize = (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1)) & ~(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(shaderRecordSize);
    CD3DX12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto CreateShaderTable = [&](const wchar_t* name, const wchar_t* exportName, ComPtr<ID3D12Resource>& out) {
        IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)));
        IE_Check(out->SetName(name));
        SetResourceBufferData(out, stateObjectProperties->GetShaderIdentifier(exportName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);
    };

    // Single-record tables
    CreateShaderTable(L"RayGenShaderTable", L"Raygen", m_RayGenShaderTable);
    CreateShaderTable(L"MissShaderTable", L"Miss", m_MissShaderTable);
    CreateShaderTable(L"HitGroupShaderTable", L"HitGroup", m_HitGroupShaderTable);

    // Setup blur pass
    {
        // 1) Allocate intermediate UAV texture (same size/format)
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_BlurTemp)));
        IE_Check(m_BlurTemp->SetName(L"BlurTemp"));

        // 2) UAV descriptor for temp
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_UavTempIdx = m_BindlessHeaps.CreateUAV(m_BlurTemp, uavDesc);

        // 3) SRV for raw shadow UAV (t0)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_SrvRawIdx = m_BindlessHeaps.CreateSRV(m_RaytracingOutput, srvDesc);

        // 4) SRV for the temp buffer (t0 in vertical pass)
        m_SrvTempIdx = m_BindlessHeaps.CreateSRV(m_BlurTemp, srvDesc);

        ComPtr<IDxcBlob> csH = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csBlurH.hlsl", {});
        ComPtr<IDxcBlob> csV = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csBlurV.hlsl", {});

        IE_Check(m_Device->CreateRootSignature(0, csH->GetBufferPointer(), csH->GetBufferSize(), IID_PPV_ARGS(&m_BlurRootSignature)));

        // Horizontal PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoH{};
        psoH.pRootSignature = m_BlurRootSignature.Get();
        psoH.CS = {csH->GetBufferPointer(), csH->GetBufferSize()};
        IE_Check(m_Device->CreateComputePipelineState(&psoH, IID_PPV_ARGS(&m_BlurHPso)));

        // Vertical PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoV{};
        psoV.pRootSignature = m_BlurRootSignature.Get();
        psoV.CS = {csV->GetBufferPointer(), csV->GetBufferSize()};
        IE_Check(m_Device->CreateComputePipelineState(&psoV, IID_PPV_ARGS(&m_BlurVPso)));
    }
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

    Shader dxShader{};
    dxShader.bytecode = CD3DX12_SHADER_BYTECODE(result->GetBufferPointer(), result->GetBufferSize());
    dxShader.filename = filename;
    dxShader.defines = defines;
    return dxShader;
}

void Renderer::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    // Create temporary upload buffer
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