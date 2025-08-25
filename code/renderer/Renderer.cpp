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
#include <execution>
#include <ffx_api/dx12/ffx_api_dx12.hpp>
#include <ffx_api/ffx_api.hpp>
#include <ffx_api/ffx_upscale.hpp>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "../common/Asserts.h"
#include "../common/CommandLineArguments.h"
#include "../window/Window.h"
#include "Camera.h"
#include "Constants.h"
#include "ScenePack.h"
#include "meshoptimizer.h"

#include <filesystem>

namespace
{
f32 g_Timing_AverageWindowMs = 2000.0f;

struct ImGuiAllocCtx
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    u32 next = 0;
    u32 capacity = 0;
};
ImGuiAllocCtx* g_ImGuiAlloc = nullptr;

f32 g_ToneMapping_WhitePoint = 1.0f;
f32 g_ToneMapping_Contrast = 1.0f;
f32 g_ToneMapping_Saturation = 1.4f;

f32 g_Camera_FOV = 60.f;
f32 g_Camera_FrustumCullingFOV = 60.f;

f32 g_Sun_Azimuth = IE_ToRadians(210.f);
f32 g_Sun_Elevation = IE_ToRadians(-48.f);
f32 g_Sun_Intensity = 1.0f;

f32 g_IBL_DiffuseIntensity = 1.0f;
f32 g_IBL_SpecularIntensity = 1.0f;
f32 g_IBL_SkyIntensity = 1.0f;

f32 g_AutoExposure_TargetPct = 0.82f;
f32 g_AutoExposure_LowReject = 0.02f;
f32 g_AutoExposure_HighReject = 0.95f;
f32 g_AutoExposure_Key = 0.22f;
f32 g_AutoExposure_MinLogLum = -3.5f;
f32 g_AutoExposure_MaxLogLum = 3.5f;
f32 g_AutoExposure_ClampMin = 1.0f / 32.0f;
f32 g_AutoExposure_ClampMax = 32.0f;
f32 g_AutoExposure_TauBright = 0.20f;
f32 g_AutoExposure_TauDark = 1.50f;

bool g_RTShadows_Enabled = true;
RayTracingResolution g_RTShadows_Type = RayTracingResolution::FullX_HalfY;
f32 g_RTShadows_IBLDiffuseIntensity = 0.20f;
f32 g_RTShadows_IBLSpecularIntensity = 0.80f;

f32 g_SSAO_SampleRadius = 0.05f;
f32 g_SSAO_SampleBias = 0.0001f;
f32 g_SSAO_Power = 1.4f;
} // namespace

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

    SetupImGui();
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
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_ImGuiAlloc = nullptr;
}

void Renderer::Render()
{
    // Get/Wait next frame
    m_FrameInFlightIdx = m_Swapchain->GetCurrentBackBufferIndex();
    PerFrameData& frameData = GetCurrentFrameData();
    WaitOnFence(frameData.frameFence, frameData.frameFenceValue);

    // Grab timings from the previous use of this frame slot
    CollectGpuTimings(frameData);
    UpdateGpuTimingAverages(Window::GetFrameTimeMs());
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
    f32 cosE = cosf(g_Sun_Elevation);
    m_SunDir = float3{cosE * cosf(g_Sun_Azimuth), sinf(g_Sun_Elevation), cosE * sinf(g_Sun_Azimuth)}.Normalized();

    float4x4 view = camera.GetViewMatrix();
    float4x4 projJ = camera.GetProjection();
    float4x4 projNoJ = camera.GetProjectionNoJitter();

    float4x4 prevViewProjNoJ = m_LastViewProjNoJ;
    float4x4 viewProjNoJ = view * projNoJ;
    m_LastViewProjNoJ = viewProjNoJ;

    float2 zNearFar = camera.GetZNearFar();

    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = {m_BindlessHeaps.m_CbvSrvUavHeap.Get(), m_BindlessHeaps.m_SamplerHeap.Get()};

    // Constant Buffer
    float4x4 projCull = camera.GetFrustumCullingProjection();
    float4x4 vp = (view * projCull).Transposed();

    VertexConstants constants{};
    constants.cameraPos = camera.GetPosition();
    constants.planes[0] = (vp[3] + vp[0]).Normalized();
    constants.planes[1] = (vp[3] - vp[0]).Normalized();
    constants.planes[2] = (vp[3] + vp[1]).Normalized();
    constants.planes[3] = (vp[3] - vp[1]).Normalized();
    constants.planes[4] = vp[2].Normalized();
    constants.planes[5] = (vp[3] - vp[2]).Normalized();
    constants.view = view;
    constants.viewProj = (view * projJ);
    constants.viewProjNoJ = viewProjNoJ;
    constants.prevViewProjNoJ = prevViewProjNoJ;
    SetResourceBufferData(m_ConstantsBuffer, &constants, sizeof(constants), m_FrameInFlightIdx * sizeof(constants));

    float4x4 invViewProj = (view * projJ).Inversed();

    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));
    ComPtr<ID3D12GraphicsCommandList7> cmd = frameData.cmd;

    // Depth Pre-Pass
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cmd->OMSetRenderTargets(0, nullptr, false, &m_DSVsHandle[m_FrameInFlightIdx]);
    cmd->ClearDepthStencilView(m_DSVsHandle[m_FrameInFlightIdx], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
    cmd->RSSetViewports(1, &m_RenderViewport);
    cmd->RSSetScissorRects(1, &m_RenderRect);

    GPU_MARKER_BEGIN(cmd, frameData, "Depth Pre-Pass - Opaque");
    {
        cmd->SetGraphicsRootSignature(m_DepthPrePassOpaqueRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));

        // Culled
        cmd->SetPipelineState(m_DepthPrePassOpaquePSO.Get());
        for (const auto& primitive : m_PrimitivesPerAlphaMode[AlphaMode_Opaque][0])
        {
            PrimitiveConstants rootConstants{};
            rootConstants.world = primitive->GetTransform();
            rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
            rootConstants.meshletCount = primitive->m_Meshlets.Size();
            rootConstants.materialIdx = primitive->m_MaterialIdx;
            rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
            rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
            rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
            rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
            rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
            rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
            cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
        }

        // No-cull (double-sided)
        cmd->SetPipelineState(m_DepthPrePassOpaquePSO_NoCull.Get());
        for (const auto& primitive : m_PrimitivesPerAlphaMode[AlphaMode_Opaque][1])
        {
            PrimitiveConstants rootConstants{};
            rootConstants.world = primitive->GetTransform();
            rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
            rootConstants.meshletCount = primitive->m_Meshlets.Size();
            rootConstants.materialIdx = primitive->m_MaterialIdx;
            rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
            rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
            rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
            rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
            rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
            rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
            cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
        }
    }
    GPU_MARKER_END(cmd, frameData);
    GPU_MARKER_BEGIN(cmd, frameData, "Depth Pre-Pass - Alpha-Tested");
    {
        cmd->SetGraphicsRootSignature(m_DepthPrePassAlphaTestRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));

        // Culled
        cmd->SetPipelineState(m_DepthPrePassAlphaTestPSO.Get());
        for (const auto& primitive : m_PrimitivesPerAlphaMode[AlphaMode_Mask][0])
        {
            PrimitiveConstants rootConstants{};
            rootConstants.world = primitive->GetTransform();
            rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
            rootConstants.meshletCount = primitive->m_Meshlets.Size();
            rootConstants.materialIdx = primitive->m_MaterialIdx;
            rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
            rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
            rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
            rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
            rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
            rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
            cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
        }

        // No-cull
        cmd->SetPipelineState(m_DepthPrePassAlphaTestPSO_NoCull.Get());
        for (const auto& primitive : m_PrimitivesPerAlphaMode[AlphaMode_Mask][1])
        {
            PrimitiveConstants rootConstants{};
            rootConstants.world = primitive->GetTransform();
            rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
            rootConstants.meshletCount = primitive->m_Meshlets.Size();
            rootConstants.materialIdx = primitive->m_MaterialIdx;
            rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
            rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
            rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
            rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
            rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
            rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
            cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
        }
    }
    GPU_MARKER_END(cmd, frameData);
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);

    if (g_RTShadows_Enabled)
    {
        Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_GENERIC_READ);

        // Shadows RayTracing
        GPU_MARKER_BEGIN(cmd, frameData, "Shadows Ray-Tracing");
        {
            cmd->SetComputeRootSignature(m_RaytracingGlobalRootSignature.Get());
            cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());

            cmd->SetPipelineState1(m_DxrStateObject.Get());

            float3 cameraPos = camera.GetPosition();
            RtShadowsTraceConstants rayTraceRootConstants{};
            rayTraceRootConstants.invViewProj = invViewProj;
            rayTraceRootConstants.outputTextureIndex = m_RaytracingOutputIndex;
            rayTraceRootConstants.tlasIndex = m_RaytracingTlasIndex;
            rayTraceRootConstants.depthSamplerIndex = m_LinearSamplerIdx;
            rayTraceRootConstants.resolutionType = static_cast<u32>(g_RTShadows_Type); // I should use defines to DXC when compiling but apparently there
                                                                                       // is an DXC issue with -D defines with raytracing shaders...
            rayTraceRootConstants.sunDir = m_SunDir;
            rayTraceRootConstants.frameIndex = m_FrameIndex;
            rayTraceRootConstants.cameraPos = cameraPos;
            rayTraceRootConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
            cmd->SetComputeRoot32BitConstants(0, sizeof(RtShadowsTraceConstants) / sizeof(u32), &rayTraceRootConstants, 0);

            uint2 rtShadowsRes = m_RenderSize;
            switch (g_RTShadows_Type)
            {
            case RayTracingResolution::Full:
                break;
            case RayTracingResolution::FullX_HalfY:
                rtShadowsRes.y /= 2;
                break;
            case RayTracingResolution::Half:
                rtShadowsRes /= 2;
                break;
            case RayTracingResolution::Quarter:
                rtShadowsRes /= 4;
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
        GPU_MARKER_END(cmd, frameData);

        // Blur RT Shadows
        GPU_MARKER_BEGIN(cmd, frameData, "Ray-Traced Shadows Blur");
        {
            RTShadowsBlurConstants rootConstants;
            rootConstants.zNear = zNearFar.x;
            rootConstants.zFar = zNearFar.y;
            rootConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];

            cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
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
        GPU_MARKER_END(cmd, frameData);

        Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_READ);
    }

    GBuffer& gb = m_GBuffers[m_FrameInFlightIdx];

    Barrier(cmd, gb.albedo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(cmd, gb.normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(cmd, gb.material, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(cmd, gb.motionVector, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    f32 clearColor[4] = {0, 0, 0, 0};
    for (u8 i = 0; i < GBuffer::targetCount; ++i)
    {
        cmd->ClearRenderTargetView(m_GBuffersRTV[m_FrameInFlightIdx][i], clearColor, 0, nullptr);
    }

    for (u32 i = 0; i < AlphaMode_Count; ++i)
    {
        const char* passName = i == 0 ? "GBuffer Opaque Pass" : i == 1 ? "GBuffer Blend Pass (Makes no sense)" : "GBuffer Masked Pass";
        GPU_MARKER_BEGIN(cmd, frameData, passName);
        {
            AlphaMode alphaMode = static_cast<AlphaMode>(i);

            cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
            cmd->SetGraphicsRootSignature(m_GBufferPassRootSigs[alphaMode].Get());
            cmd->OMSetRenderTargets(GBuffer::targetCount, m_GBuffersRTV[m_FrameInFlightIdx].Data(), false, &m_DSVsHandle[m_FrameInFlightIdx]);
            cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(VertexConstants));

            // Culled (ds=0)
            cmd->SetPipelineState(m_GBufferPassPSOs[alphaMode][0].Get());
            for (const auto& primitive : m_PrimitivesPerAlphaMode[alphaMode][0])
            {
                PrimitiveConstants rootConstants{};
                rootConstants.world = primitive->GetTransform();
                rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
                rootConstants.meshletCount = primitive->m_Meshlets.Size();
                rootConstants.materialIdx = primitive->m_MaterialIdx;
                rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
                rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
                rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
                rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
                rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
                rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
                cmd->SetGraphicsRoot32BitConstants(0, sizeof(PrimitiveConstants) / 4, &rootConstants, 0);
                cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
            }

            // No-cull (ds=1)
            cmd->SetPipelineState(m_GBufferPassPSOs[alphaMode][1].Get());
            for (const auto& primitive : m_PrimitivesPerAlphaMode[alphaMode][1])
            {
                PrimitiveConstants rootConstants{};
                rootConstants.world = primitive->GetTransform();
                rootConstants.worldIT = rootConstants.world.Inversed().Transposed();
                rootConstants.meshletCount = primitive->m_Meshlets.Size();
                rootConstants.materialIdx = primitive->m_MaterialIdx;
                rootConstants.verticesBufferIndex = primitive->m_VerticesBuffer->srvIndex;
                rootConstants.meshletsBufferIndex = primitive->m_MeshletsBuffer->srvIndex;
                rootConstants.meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->srvIndex;
                rootConstants.meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->srvIndex;
                rootConstants.meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->srvIndex;
                rootConstants.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
                cmd->SetGraphicsRoot32BitConstants(0, sizeof(PrimitiveConstants) / 4, &rootConstants, 0);
                cmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
            }
        }
        GPU_MARKER_END(cmd, frameData);
    }

    Barrier(cmd, gb.albedo, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.normal, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.material, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.motionVector, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, m_DSVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    GPU_MARKER_BEGIN(cmd, frameData, "SSAO Pass");
    {
        Barrier(cmd, m_SSAOTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_SSAOPso.Get());
        cmd->SetComputeRootSignature(m_SSAORootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());

        SSAOConstants ssaoConstants{};
        ssaoConstants.radius = g_SSAO_SampleRadius;
        ssaoConstants.bias = g_SSAO_SampleBias;
        ssaoConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
        ssaoConstants.normalTextureIndex = gb.normalIndex;
        ssaoConstants.proj = projJ;
        ssaoConstants.invProj = projJ.Inversed();
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
    GPU_MARKER_END(cmd, frameData);

    Barrier(cmd, gb.albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.material, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, gb.motionVector, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    GPU_MARKER_BEGIN(cmd, frameData, "Lighting Pass");
    {
        LightingPassConstants lightingPassConstants{};
        lightingPassConstants.albedoTextureIndex = m_GBuffers[m_FrameInFlightIdx].albedoIndex;
        lightingPassConstants.normalTextureIndex = m_GBuffers[m_FrameInFlightIdx].normalIndex;
        lightingPassConstants.materialTextureIndex = m_GBuffers[m_FrameInFlightIdx].materialIndex;
        lightingPassConstants.depthTextureIndex = m_DSVsIdx[m_FrameInFlightIdx];
        lightingPassConstants.samplerIndex = m_LinearSamplerIdx;
        lightingPassConstants.cameraPos = camera.GetPosition();
        lightingPassConstants.view = view;
        lightingPassConstants.invView = view.Inversed();
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
        memcpy(m_LightingCBVMapped[m_FrameInFlightIdx], &lightingPassConstants, sizeof(lightingPassConstants));

        cmd->OMSetRenderTargets(1, &m_HDR_RTVsHandle[m_FrameInFlightIdx], false, nullptr);
        cmd->SetPipelineState(m_LightingPassPSO.Get());
        cmd->SetGraphicsRootSignature(m_LightingPassRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
        cmd->SetGraphicsRootConstantBufferView(0, m_LightingCBVs[m_FrameInFlightIdx]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData);

    cmd->RSSetViewports(1, &m_PresentViewport);
    cmd->RSSetScissorRects(1, &m_PresentRect);

    GPU_MARKER_BEGIN(cmd, frameData, "FSR");
    {
        ffx::DispatchDescUpscale dispatchUpscale{};

        FfxApiResource hdrColor{};
        hdrColor.resource = m_HDR_RTVs[m_FrameInFlightIdx].Get();
        hdrColor.description = {FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, m_RenderSize.x, m_RenderSize.y, 1, 1, 0, 0};
        hdrColor.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
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
        dispatchUpscale.motionVectorScale.x = m_RenderSize.x;
        dispatchUpscale.motionVectorScale.y = m_RenderSize.y;
        dispatchUpscale.frameTimeDelta = Window::GetFrameTimeMs();
        dispatchUpscale.renderSize.width = m_RenderSize.x;
        dispatchUpscale.renderSize.height = m_RenderSize.y;
        dispatchUpscale.preExposure = 1.0f;

        IE_Assert(ffx::Dispatch(m_UpscalingContext, dispatchUpscale) == ffx::ReturnCode::Ok);
    }
    GPU_MARKER_END(cmd, frameData);

    UAVBarrier(cmd, m_FsrOutputs[m_FrameInFlightIdx]);

    GPU_MARKER_BEGIN(cmd, frameData, "Histogram Pass");
    {
        // Clear
        ClearConstants clearConstants{};
        clearConstants.bufferIndex = m_HistogramBuffer->uavIndex;
        clearConstants.numElements = m_HistogramBuffer->numElements;
        cmd->SetPipelineState(m_ClearUintPso.Get());
        cmd->SetComputeRootSignature(m_ClearUintRootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
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
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
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
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
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
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
        cmd->SetComputeRoot32BitConstants(0, sizeof(adaptExposureConstants) / 4, &adaptExposureConstants, 0);
        cmd->Dispatch(1, 1, 1);

        UAVBarrier(cmd, m_AdaptExposureBuffer->buffer);
    }
    GPU_MARKER_END(cmd, frameData);

    // Tone mapping pass
    GPU_MARKER_BEGIN(cmd, frameData, "Tone Mapping");
    {
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
        cmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(toneMappingRootConstants) / 4, &toneMappingRootConstants, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData);

    // ImGui
    GPU_MARKER_BEGIN(cmd, frameData, "ImGui");
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y), ImGuiCond_Always);
        ImGuiWindowFlags statsFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::Begin("Stats", nullptr, statsFlags);
        ImGui::Text("FPS: %u", static_cast<u32>(Window::GetFPS()));
        float3 cameraPos = camera.GetPosition();
        ImGui::Text("Camera pos: %.1f  %.1f  %.1f", cameraPos.x, cameraPos.y, cameraPos.z);
        ImGui::Separator();

        ImGui::SliderFloat("GPU timing avg window (ms)", &g_Timing_AverageWindowMs, 0.0f, 5000.0f, "%.0f");

        // precompute total of smoothed times
        double totalAvg = 0.0;
        for (u32 i = 0; i < m_LastGpuTimingCount; ++i)
        {
            const char* name = m_LastGpuTimings[i].name;
            double avg = m_LastGpuTimings[i].ms; // fallback
            for (u32 j = 0; j < m_GpuTimingSmoothCount; ++j)
            {
                if (m_GpuTimingSmooth[j].name == name)
                {
                    avg = m_GpuTimingSmooth[j].value;
                    break;
                }
            }
            totalAvg += avg;
        }

        if (ImGui::BeginTable("GpuTimingsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("% of total", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();

            for (u32 i = 0; i < m_LastGpuTimingCount; ++i)
            {
                const char* name = m_LastGpuTimings[i].name;
                double avg = m_LastGpuTimings[i].ms; // fallback

                for (u32 j = 0; j < m_GpuTimingSmoothCount; ++j)
                {
                    if (m_GpuTimingSmooth[j].name == name)
                    {
                        avg = m_GpuTimingSmooth[j].value;
                        break;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", static_cast<float>(avg));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f%%", (totalAvg > 0.0) ? static_cast<float>(avg * 100.0 / totalAvg) : 0.0f);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Total (sum of listed)");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<float>(totalAvg));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", (totalAvg > 0.0) ? "100.0%" : "0.0%");

            ImGui::EndTable();
        }

        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGuiWindowFlags settingsFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::Begin("Settings", nullptr, settingsFlags);

        if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("White Point", &g_ToneMapping_WhitePoint, 0.0f, 32.0f);
            ImGui::SliderFloat("Contrast", &g_ToneMapping_Contrast, 0.0f, 3.0f);
            ImGui::SliderFloat("Saturation", &g_ToneMapping_Saturation, 0.0f, 3.0f);
        }

        if (ImGui::CollapsingHeader("Auto-Exposure", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Target %", &g_AutoExposure_TargetPct, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Low Reject", &g_AutoExposure_LowReject, 0.0f, 0.2f, "%.2f");
            ImGui::SliderFloat("High Reject", &g_AutoExposure_HighReject, 0.8f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grey (Key)", &g_AutoExposure_Key, 0.05f, 0.50f, "%.2f");
            ImGui::SliderFloat("Min Log Lum", &g_AutoExposure_MinLogLum, -16.0f, 0.0f, "%.1f");
            ImGui::SliderFloat("Max Log Lum", &g_AutoExposure_MaxLogLum, 0.0f, 16.0f, "%.1f");
            ImGui::SliderFloat("Light Adapt Time (s)", &g_AutoExposure_TauBright, 0.05f, 0.5f, "%.2f");
            ImGui::SliderFloat("Dark Adapt Time (s)", &g_AutoExposure_TauDark, 0.5f, 6.0f, "%.2f");
            ImGui::SliderFloat("Clamp Min", &g_AutoExposure_ClampMin, 1.0f / 256.0f, 1.0f, "%.5f");
            ImGui::SliderFloat("Clamp Max", &g_AutoExposure_ClampMax, 1.0f, 256.0f, "%.1f");
        }

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("FOV", &g_Camera_FOV, 10.0f, 120.0f);
            ImGui::SliderFloat("Frustum Culling FOV", &g_Camera_FrustumCullingFOV, 10.0f, 120.0f);
        }

        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderAngle("Azimuth", &g_Sun_Azimuth, 0.0f, 360.0f);
            ImGui::SliderAngle("Elevation", &g_Sun_Elevation, -89.0f, 89.0f);
            ImGui::SliderFloat("Intensity", &g_Sun_Intensity, 0.f, 8.f);
        }

        if (ImGui::CollapsingHeader("IBL", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Diffuse Intensity", &g_IBL_DiffuseIntensity, 0.f, 2.f);
            ImGui::SliderFloat("Specular Intensity", &g_IBL_SpecularIntensity, 0.f, 2.f);
            ImGui::SliderFloat("Sky Intensity", &g_IBL_SkyIntensity, 0.f, 2.f);
        }

        if (ImGui::CollapsingHeader("RT Shadows", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("RT Shadows Enabled", &g_RTShadows_Enabled);

            const char* RTResLabels[] = {"Full", "FullX_HalfY", "Half", "Quarter"};
            static int currentRTRes = static_cast<int>(g_RTShadows_Type);
            if (ImGui::Combo("Ray-trace Resolution", &currentRTRes, RTResLabels, IM_ARRAYSIZE(RTResLabels)))
            {
                g_RTShadows_Type = static_cast<RayTracingResolution>(currentRTRes);
            }

            ImGui::SliderFloat("IBL Diffuse Intensity", &g_RTShadows_IBLDiffuseIntensity, 0.f, 1.f);
            ImGui::SliderFloat("IBL Specular Intensity", &g_RTShadows_IBLSpecularIntensity, 0.f, 1.f);
        }

        if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Sample Radius", &g_SSAO_SampleRadius, 0.f, 0.1f, "%.6f");
            ImGui::SliderFloat("Sample Bias", &g_SSAO_SampleBias, 0.f, 0.001f, "%.8f");
            ImGui::SliderFloat("Power", &g_SSAO_Power, 0.f, 3.f);
        }

        ImGui::End();

        ImGui::Render();

        cmd->OMSetRenderTargets(1, &m_RTVsHandle[m_FrameInFlightIdx], false, nullptr);
        ID3D12DescriptorHeap* heaps[] = {m_ImGuiSrvHeap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd.Get());

        Barrier(cmd, m_RTVs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    GPU_MARKER_END(cmd, frameData);

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

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, m_RenderSize.x, m_RenderSize.y, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
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
    IE_Check(m_CommandQueue->GetTimestampFrequency(&m_TimestampFrequency));
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

void Renderer::CollectGpuTimings(PerFrameData& frameData)
{
    m_LastGpuTimingCount = 0;
    if (!frameData.gpuTimers.nextIdx || !m_TimestampFrequency)
    {
        return;
    }

    f64 to_ms = 1000.0 / static_cast<f64>(m_TimestampFrequency);

    u64* ticks = nullptr;
    D3D12_RANGE r{0, frameData.gpuTimers.nextIdx * (u32)sizeof(u64)};
    IE_Check(frameData.gpuTimers.readback->Map(0, &r, (void**)&ticks));

    for (u32 i = 0; i < frameData.gpuTimers.passCount && i < 128; ++i)
    {
        const GpuTimers::Pass& p = frameData.gpuTimers.passes[i];
        if (p.idxEnd <= p.idxBegin)
        {
            continue;
        }
        u64 t0 = ticks[p.idxBegin];
        u64 t1 = ticks[p.idxEnd];
        f64 dt = static_cast<f64>(t1 - t0) * to_ms;
        m_LastGpuTimings[m_LastGpuTimingCount++] = {p.name, dt};
    }

    D3D12_RANGE w{0, 0};
    frameData.gpuTimers.readback->Unmap(0, &w);
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
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPrePassOpaquePSO)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPrePassOpaquePSO_NoCull)));
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
        IE_Check(m_Device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPrePassAlphaTestPSO)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(m_Device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPrePassAlphaTestPSO_NoCull)));
    }
}

void Renderer::CreateGBufferPassResources(const Vector<WString>& globalDefines)
{
    DXGI_FORMAT formats[GBuffer::targetCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM, // Albedo
        DXGI_FORMAT_R16G16_FLOAT,   // Normal
        DXGI_FORMAT_R8G8_UNORM,     // Material
        DXGI_FORMAT_R16G16_FLOAT,   // Motion vector
    };
    const wchar_t* rtvNames[GBuffer::targetCount] = {L"GBuffer Albedo", L"GBuffer Normal", L"GBuffer Material", L"GBuffer Motion Vector"};

    m_PixelShader[AlphaMode_Opaque] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", {globalDefines});
    m_PixelShader[AlphaMode_Blend] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", {globalDefines, L"ENABLE_BLEND"});
    m_PixelShader[AlphaMode_Mask] = LoadShader(IE_SHADER_TYPE_PIXEL, L"psGBuffer.hlsl", {globalDefines, L"ENABLE_ALPHA_TEST"});

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        GBuffer& gbuff = m_GBuffers[i];
        ComPtr<ID3D12Resource>* targets[GBuffer::targetCount] = {&gbuff.albedo, &gbuff.normal, &gbuff.material, &gbuff.motionVector};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 4;
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

        // PSO [0] = culled
        {
            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(msDesc);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBufferPassPSOs[alphaMode][0])));
        }

        // PSO [1] = no cull
        {
            D3DX12_MESH_SHADER_PIPELINE_STATE_DESC noCull = msDesc;
            D3D12_RASTERIZER_DESC rast = noCull.RasterizerState;
            rast.CullMode = D3D12_CULL_MODE_NONE;
            noCull.RasterizerState = rast;

            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(noCull);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(m_Device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBufferPassPSOs[alphaMode][1])));
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

    // SRVs
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_HDRSrvIdx[i] = m_BindlessHeaps.CreateSRV(m_HDR_RTVs[i], srvDesc);
    }
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

void Renderer::SetupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.Fonts->Clear();
    ImFont* myFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 21.0f);
    IM_ASSERT(myFont != nullptr);
    io.FontDefault = myFont;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(Window::GetInstance().GetHwnd());

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 128;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    IE_Check(m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_ImGuiSrvHeap)));

    static ImGuiAllocCtx sCtx;
    sCtx.cpu = m_ImGuiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    sCtx.gpu = m_ImGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    sCtx.capacity = desc.NumDescriptors;
    sCtx.next = 0;
    g_ImGuiAlloc = &sCtx;

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_Device.Get();
    initInfo.CommandQueue = m_CommandQueue.Get();
    initInfo.NumFramesInFlight = IE_Constants::frameInFlightCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = m_ImGuiSrvHeap.Get();
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        IM_ASSERT(g_ImGuiAlloc && "ImGui DX12 allocator context not set");
        IM_ASSERT(g_ImGuiAlloc->next < g_ImGuiAlloc->capacity && "ImGui SRV heap exhausted");
        const u32 inc = info->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        outCpu->ptr = g_ImGuiAlloc->cpu.ptr + (static_cast<size_t>(g_ImGuiAlloc->next) * inc);
        outGpu->ptr = g_ImGuiAlloc->gpu.ptr + (static_cast<size_t>(g_ImGuiAlloc->next) * inc);
        g_ImGuiAlloc->next++;
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&initInfo);
    ImGui_ImplDX12_CreateDeviceObjects();
}

void Renderer::LoadScene()
{
    PerFrameData& frameData = GetCurrentFrameData();

    ComPtr<ID3D12GraphicsCommandList7> cmd;
    IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameData.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));

    const CommandLineArguments& args = GetCommandLineArguments();
    String sceneFile = args.sceneFile;
    if (sceneFile.Empty())
    {
        sceneFile = "San-Miguel";
    }

    String scenePath = String("data/scenes/") + sceneFile + "/" + sceneFile + ".glb";

    Camera& camera = Camera::GetInstance();
    camera.LoadSceneConfig(sceneFile);

    {
        namespace fs = std::filesystem;

        fs::path fsScenePath = fs::path(scenePath.Data());
        fs::path baseDir = fsScenePath.parent_path();
        fs::path packPath = baseDir / (fsScenePath.stem().wstring() + L".iskurpack");

        ScenePack::Get().Open(packPath);
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    // Remove image loader
    loader.SetImageLoader(
        [](tinygltf::Image* img, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*) {
            img->image.clear();
            img->width = img->height = img->component = img->bits = 0;
            return true;
        },
        nullptr);
    std::string err;
    std::string warn;
    IE_Assert(loader.LoadBinaryFromFile(&model, &err, &warn, scenePath.Data()));

    IE_Assert(warn.empty());
    IE_Assert(err.empty());
    m_World = IE_MakeSharedPtr<World>(model);

    // >> Store all textures
    // Because the textures are the first thing being uploaded to the heap, we can keep their gltf indices!
    {
        using namespace DirectX;
        namespace fs = std::filesystem;

        fs::path fsScenePath = fs::path(scenePath.Data());
        fs::path baseDir = fsScenePath.parent_path();

        // Load all textures first so SRV indices == glTF texture indices
        ResourceUploadBatch batch(m_Device.Get());
        batch.Begin();

        for (size_t t = 0; t < model.textures.size(); ++t)
        {
            const tinygltf::Texture& tex = model.textures[t];
            const int imgIndex = tex.source;
            IE_Assert(imgIndex >= 0 && imgIndex < static_cast<int>(model.images.size()));

            fs::path ddsPath = baseDir / L"textures" / (std::to_wstring(imgIndex) + L".dds");

            ComPtr<ID3D12Resource> res;
            IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, ddsPath.c_str(), &res, false, 0, nullptr, nullptr));

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = res->GetDesc().Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = UINT32_MAX;
            m_BindlessHeaps.CreateSRV(res, srv);
            m_Textures.Add(res);
        }

        // Finish the async uploads of DDS textures
        batch.End(m_CommandQueue.Get()).wait();
    }
    // << Store all textures

    // >>> Store all samplers
    for (const tinygltf::Sampler& sampler : model.samplers)
    {
        D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        if (sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
        {
            if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST)
            {
                filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            }
            else if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)
            {
                filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
            }
        }
        else if (sampler.magFilter == TINYGLTF_TEXTURE_FILTER_LINEAR)
        {
            if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST)
            {
                filter = D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
            }
            else if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)
            {
                filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            }
        }

        const auto MapAddressMode = [](i32 gltfAddressMode) {
            switch (gltfAddressMode)
            {
            case TINYGLTF_TEXTURE_WRAP_REPEAT:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            default:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            }
        };

        D3D12_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = filter;
        samplerDesc.AddressU = MapAddressMode(sampler.wrapS);
        samplerDesc.AddressV = MapAddressMode(sampler.wrapT);
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        m_BindlessHeaps.CreateSampler(samplerDesc);
    }
    // <<< Store all samplers

    // >>> Store all materials
    for (const tinygltf::Material& material : model.materials)
    {
        Material m;

        const tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;
        if (!pbr.baseColorFactor.empty())
        {
            m.baseColorFactor = {static_cast<f32>(pbr.baseColorFactor[0]), static_cast<f32>(pbr.baseColorFactor[1]), static_cast<f32>(pbr.baseColorFactor[2]),
                                 static_cast<f32>(pbr.baseColorFactor[3])};
        }
        else
        {
            m.baseColorFactor = {1, 1, 1, 1};
        }
        m.metallicFactor = static_cast<f32>(pbr.metallicFactor);
        m.roughnessFactor = static_cast<f32>(pbr.roughnessFactor);

        m.baseColorTextureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
        {
            m.baseColorSamplerIndex = model.textures[material.pbrMetallicRoughness.baseColorTexture.index].sampler;
        }
        else
        {
            m.baseColorSamplerIndex = -1;
        }

        m.metallicRoughnessTextureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
        {
            m.metallicRoughnessSamplerIndex = model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].sampler;
        }
        else
        {
            m.metallicRoughnessSamplerIndex = -1;
        }

        m.normalTextureIndex = material.normalTexture.index;
        m.normalScale = static_cast<float>(material.normalTexture.scale);

        if (material.normalTexture.index != -1)
        {
            m.normalSamplerIndex = model.textures[material.normalTexture.index].sampler;
            IE_Assert(m.normalSamplerIndex >= 0);
        }
        else
        {
            m.normalSamplerIndex = -1;
        }

        m.alphaMode = AlphaMode_Opaque;
        if (material.alphaMode == "BLEND")
        {
            m.alphaMode = AlphaMode_Mask; // TODO: using Mask for now since Blend is not implemented
        }
        if (material.alphaMode == "MASK")
        {
            m.alphaMode = AlphaMode_Mask;
        }
        m.alphaCutoff = static_cast<float>(material.alphaCutoff);
        m.doubleSided = material.doubleSided;

        m_Materials.Add(m);
    }

    if (!m_Materials.Empty())
    {
        m_MaterialsBuffer = CreateStructuredBuffer(m_Materials.ByteSize(), sizeof(Material), L"Materials");
        SetBufferData(cmd, m_MaterialsBuffer, m_Materials.Data(), m_Materials.ByteSize(), 0);
    }
    // <<< Store all materials

    // >>> Upload primitives to GPU
    const Vector<SharedPtr<Primitive>>& primitives = m_World->GetPrimitives();
    for (const SharedPtr<Primitive>& primitive : primitives)
    {
        primitive->m_VerticesBuffer = CreateStructuredBuffer(primitive->m_Vertices.ByteSize(), sizeof(Vertex), L"Vertices");
        primitive->m_MeshletsBuffer = CreateBytesBuffer(primitive->m_Meshlets.ByteSize(), L"Meshlets");
        primitive->m_MeshletVerticesBuffer = CreateStructuredBuffer(primitive->m_MeshletVertices.ByteSize(), sizeof(u32), L"Meshlet Vertices");
        primitive->m_MeshletTrianglesBuffer = CreateBytesBuffer(primitive->m_MeshletTriangles.Size(), L"Meshlet Triangles");
        primitive->m_MeshletBoundsBuffer = CreateStructuredBuffer(primitive->m_MeshletBounds.ByteSize(), sizeof(meshopt_Bounds), L"Meshlet Bounds");

        SetBufferData(cmd, primitive->m_VerticesBuffer, primitive->m_Vertices.Data(), primitive->m_Vertices.ByteSize(), 0);
        SetBufferData(cmd, primitive->m_MeshletsBuffer, primitive->m_Meshlets.Data(), primitive->m_Meshlets.ByteSize(), 0);
        SetBufferData(cmd, primitive->m_MeshletVerticesBuffer, primitive->m_MeshletVertices.Data(), primitive->m_MeshletVertices.ByteSize(), 0);
        SetBufferData(cmd, primitive->m_MeshletTrianglesBuffer, primitive->m_MeshletTriangles.Data(), primitive->m_MeshletTriangles.ByteSize(), 0);
        SetBufferData(cmd, primitive->m_MeshletBoundsBuffer, primitive->m_MeshletBounds.Data(), primitive->m_MeshletBounds.ByteSize(), 0);
    }

    for (const SharedPtr<Primitive>& primitive : primitives)
    {
        const u32 alphaMode = m_Materials[primitive->m_MaterialIdx].alphaMode;
        const u32 doubleSided = m_Materials[primitive->m_MaterialIdx].doubleSided ? 1u : 0u;

        m_PrimitivesPerAlphaMode[alphaMode][doubleSided].Add(primitive);
        m_Primitives.Add(primitive);
    }
    // <<< Upload primitives to GPU

    // >>> Create Depth SRV and Sampler
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        m_DSVsIdx[i] = m_BindlessHeaps.CreateSRV(m_DSVs[i], srvDesc);
    }

    D3D12_SAMPLER_DESC linearClampDesc{};
    linearClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearClampDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.MaxAnisotropy = 1;
    linearClampDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    linearClampDesc.MaxLOD = D3D12_FLOAT32_MAX;
    m_LinearSamplerIdx = m_BindlessHeaps.CreateSampler(linearClampDesc);
    // <<< Create Depth SRV and Sampler

    // >>> Setup Raytracing
    SetupRaytracing(cmd);
    // <<< Setup Raytracing

    // ENVMAP
    {
        WString envName = L"kloofendal_48d_partly_cloudy_puresky";
        WString basePath = L"data/textures/" + envName;

        auto loadEnvMap = [&](const WString& filePath, const wchar_t* resourceName, DXGI_FORMAT format, D3D12_SRV_DIMENSION viewDim, ComPtr<ID3D12Resource>& outTex, u32& outSrvIdx) {
            DirectX::ResourceUploadBatch batch(m_Device.Get());
            batch.Begin();

            IE_Check(CreateDDSTextureFromFile(m_Device.Get(), batch, filePath.Data(), &outTex, false, 0, nullptr, nullptr));

            IE_Check(outTex->SetName(resourceName));

            // 2) finish the upload on GPU
            batch.End(m_CommandQueue.Get()).wait();

            // 3) build a minimal SRV‐desc
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = viewDim;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (viewDim == D3D12_SRV_DIMENSION_TEXTURECUBE)
            {
                srv.TextureCube.MipLevels = UINT32_MAX;
            }
            else // TEXTURE2D
            {
                srv.Texture2D.MipLevels = UINT32_MAX;
            }

            outSrvIdx = m_BindlessHeaps.CreateSRV(outTex, srv);
        };

        loadEnvMap(basePath + L"/envMap.dds", L"EnvCubeMap", DXGI_FORMAT_BC6H_UF16, D3D12_SRV_DIMENSION_TEXTURECUBE, m_EnvCubeMap, m_EnvCubeMapSrvIdx);
        loadEnvMap(basePath + L"/diffuseIBL.dds", L"DiffuseIBL", DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_SRV_DIMENSION_TEXTURECUBE, m_DiffuseIBL, m_DiffuseIBLIdx);
        loadEnvMap(basePath + L"/specularIBL.dds", L"SpecularIBL", DXGI_FORMAT_BC6H_UF16, D3D12_SRV_DIMENSION_TEXTURECUBE, m_SpecularIBL, m_SpecularIBLIdx);
        loadEnvMap(L"data/textures/BRDF_LUT.dds", L"BrdfLut", DXGI_FORMAT_R16G16_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, m_BrdfLUT, m_BrdfLUTIdx);
    }

    IE_Check(cmd->Close());
    ID3D12CommandList* pCmd = cmd.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmd);

    u64 fenceToWait = ++frameData.frameFenceValue;
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), fenceToWait));

    // Badly wait that the setup command has finished
    while (frameData.frameFence->GetCompletedValue() < fenceToWait)
    {
    }
    cmd.Reset();
    m_InFlightUploads.Clear();
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
    for (const SharedPtr<Primitive>& prim : m_Primitives)
    {
        AllocateUploadBuffer(prim->m_Vertices.Data(), prim->m_Vertices.ByteSize(), 0, prim->m_VertexBuffer, prim->m_VertexBufferAlloc, L"Primitive/VertexBuffer");
        AllocateUploadBuffer(prim->m_Indices.Data(), prim->m_Indices.ByteSize(), 0, prim->m_IndexBuffer, prim->m_IndexBufferAlloc, L"Primitive/IndexBuffer");

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometryDesc.Triangles.Transform3x4 = 0;
        geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDesc.Triangles.IndexCount = prim->m_Indices.Size();
        geometryDesc.Triangles.VertexCount = prim->m_Vertices.Size();
        geometryDesc.Triangles.IndexBuffer = prim->m_IndexBuffer->GetGPUVirtualAddress();
        geometryDesc.Triangles.VertexBuffer.StartAddress = prim->m_VertexBuffer->GetGPUVirtualAddress();
        geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuildInfo;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blasInputs.NumDescs = 1;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.pGeometryDescs = &geometryDesc;
        m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuildInfo);
        IE_Assert(blasPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        AllocateUAVBuffer(static_cast<u32>(blasPrebuildInfo.ResultDataMaxSizeInBytes), prim->m_BLAS, prim->m_BLASAlloc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS");
        AllocateUAVBuffer(static_cast<u32>(blasPrebuildInfo.ScratchDataSizeInBytes), prim->m_ScratchResource, prim->m_ScratchResourceAlloc, D3D12_RESOURCE_STATE_COMMON, L"ScratchResource");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuildDesc{};
        blasBuildDesc.DestAccelerationStructureData = prim->m_BLAS->GetGPUVirtualAddress();
        blasBuildDesc.Inputs = blasInputs;
        blasBuildDesc.ScratchAccelerationStructureData = prim->m_ScratchResource->GetGPUVirtualAddress();
        cmdList->BuildRaytracingAccelerationStructure(&blasBuildDesc, 0, nullptr);

        float4x4 transform = prim->GetTransform();
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc{};
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = prim->m_BLAS->GetGPUVirtualAddress();
        instanceDesc.Transform[0][0] = transform[0][0];
        instanceDesc.Transform[0][1] = transform[1][0];
        instanceDesc.Transform[0][2] = transform[2][0];
        instanceDesc.Transform[0][3] = transform[3][0];
        instanceDesc.Transform[1][0] = transform[0][1];
        instanceDesc.Transform[1][1] = transform[1][1];
        instanceDesc.Transform[1][2] = transform[2][1];
        instanceDesc.Transform[1][3] = transform[3][1];
        instanceDesc.Transform[2][0] = transform[0][2];
        instanceDesc.Transform[2][1] = transform[1][2];
        instanceDesc.Transform[2][2] = transform[2][2];
        instanceDesc.Transform[2][3] = transform[3][2];
        instanceDescs.Add(instanceDesc);
    }

    for (const SharedPtr<Primitive>& prim : m_Primitives)
    {
        UAVBarrier(cmdList, prim->m_BLAS);
    }

    AllocateUploadBuffer(instanceDescs.Data(), instanceDescs.ByteSize(), 0, m_InstanceDescs, m_InstanceDescsAlloc, L"InstanceDescs");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    topLevelInputs.NumDescs = instanceDescs.Size();
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

    // Ray gen shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_RayGenShaderTable)));
    IE_Check(m_RayGenShaderTable->SetName(L"RayGenShaderTable"));
    SetResourceBufferData(m_RayGenShaderTable, stateObjectProperties->GetShaderIdentifier(L"Raygen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);

    // Miss shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_MissShaderTable)));
    IE_Check(m_MissShaderTable->SetName(L"MissShaderTable"));
    SetResourceBufferData(m_MissShaderTable, stateObjectProperties->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);

    // Hit group shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_HitGroupShaderTable)));
    IE_Check(m_HitGroupShaderTable->SetName(L"HitGroupShaderTable"));
    SetResourceBufferData(m_HitGroupShaderTable, stateObjectProperties->GetShaderIdentifier(L"HitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);

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

void Renderer::UpdateGpuTimingAverages(float dtMs)
{
    float window = g_Timing_AverageWindowMs;
    float alpha = (window <= 0.0f) ? 1.0f : (dtMs / window);
    if (alpha > 1.0f)
    {
        alpha = 1.0f;
    }
    if (alpha < 0.0f)
    {
        alpha = 0.0f;
    }

    for (u32 i = 0; i < m_LastGpuTimingCount; ++i)
    {
        const char* name = m_LastGpuTimings[i].name;
        double sample = m_LastGpuTimings[i].ms;

        u32 j = 0;
        for (; j < m_GpuTimingSmoothCount; ++j)
        {
            if (m_GpuTimingSmooth[j].name == name)
            {
                break;
            }
        }

        if (j == m_GpuTimingSmoothCount && j < 128)
        {
            m_GpuTimingSmooth[j].name = name;
            m_GpuTimingSmooth[j].value = sample;
            m_GpuTimingSmooth[j].initialized = true;
            m_GpuTimingSmoothCount++;
            continue;
        }

        auto& s = m_GpuTimingSmooth[j];
        if (!s.initialized)
        {
            s.value = sample;
            s.initialized = true;
        }
        else
        {
            s.value += (sample - s.value) * alpha;
        }
    }
}

SharedPtr<Buffer> Renderer::CreateStructuredBuffer(u32 sizeInBytes, u32 strideInBytes, const WString& name, D3D12_HEAP_TYPE heapType)
{
    SharedPtr<Buffer> buffer = IE_MakeSharedPtr<Buffer>();

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType;

    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, buffer->allocation.ReleaseAndGetAddressOf(),
                                         IID_PPV_ARGS(buffer->buffer.ReleaseAndGetAddressOf())));
    IE_Check(buffer->buffer->SetName(name.Data()));

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
    IE_Check(buffer->buffer->SetName(name.Data()));

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
        definesStrings.Add(L"-D" + define);
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

    m_InFlightUploads.Add({uploadBuffer, allocation});
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