// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Renderer.h"

#include "Camera.h"
#include "Constants.h"
#include "Culling.h"
#include "DLSS.h"
#include "ImGui.h"
#include "PipelineHelpers.h"
#include "Raytracing.h"
#include "SceneLoader.h"
#include "Streamline.h"
#include "TemporalJitter.h"
#include "common/CommandLineArguments.h"
#include "window/Window.h"

#include <cmath>

namespace
{
u32 GetDLSSJitterPhaseCount(const DLSS::Mode mode)
{
    switch (mode)
    {
    case DLSS::Mode::DLAA:
        return 32;
    case DLSS::Mode::Quality:
        return 48;
    case DLSS::Mode::Balanced:
        return 64;
    case DLSS::Mode::Performance:
        return 96;
    case DLSS::Mode::UltraPerformance:
        return 144;
    }

    IE_Assert(false);
    return 8;
}

f32 GetDLSSMaterialTextureMipBias(const XMUINT2 renderSize, const XMUINT2 outputSize, const f32 biasScale)
{
    if (renderSize.x == 0u || outputSize.x == 0u)
    {
        return 0.0f;
    }

    const f32 scaleX = static_cast<f32>(renderSize.x) / static_cast<f32>(outputSize.x);
    return std::log2(scaleX) * biasScale;
}

bool MatchesTexture(ID3D12Resource* resource, const DXGI_FORMAT expectedFormat, const XMUINT2& expectedSize)
{
    if (resource == nullptr)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == expectedFormat && desc.Width == expectedSize.x && desc.Height == expectedSize.y;
}

void ReleaseSrv(BindlessHeaps& bindlessHeaps, u32& srvIndex)
{
    bindlessHeaps.FreeCbvSrvUav(srvIndex);
    srvIndex = UINT32_MAX;
}

void ReleaseTextureDescriptors(BindlessHeaps& bindlessHeaps, Texture& texture)
{
    bindlessHeaps.FreeCbvSrvUav(texture.srvIndex);
    bindlessHeaps.FreeCbvSrvUav(texture.uavIndex);
    texture.srvIndex = UINT32_MAX;
    texture.uavIndex = UINT32_MAX;
}
} // namespace

Renderer::Renderer(Window& window)
    : m_Window(window), m_RenderDevice(), m_BindlessHeaps(), m_SceneResources(m_RenderDevice, m_BindlessHeaps), m_Camera(window), m_Raytracing(m_RenderDevice, m_BindlessHeaps, m_Camera)
{
}

void Renderer::Init()
{
    m_Camera.Init();

    m_Upscale.presentSize = m_Window.GetResolution();
    m_RenderDevice.Init(m_Window, m_Upscale.presentSize);
    Streamline::InitPCLForCurrentThread(GetCurrentThreadId());
    Streamline::ApplyReflexOptions();
    Shader::VerifyRequiredShaderModelSupport(m_RenderDevice.GetDevice());
    SetRenderAndPresentSize();

    m_BindlessHeaps.Init(m_RenderDevice.GetDevice());

    CreateRTVs();
    CreateDSV();

    m_ConstantsCbStride = IE_AlignUp(sizeof(VertexConstants), 256);
    BufferCreateDesc d{};
    d.sizeInBytes = m_ConstantsCbStride * IE_Constants::frameInFlightCount;
    d.heapType = D3D12_HEAP_TYPE_UPLOAD;
    d.viewKind = BufferCreateDesc::ViewKind::None;
    d.createSRV = false;
    d.createUAV = false;
    d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.name = L"Color Constants";
    m_ConstantsBuffer = CreateBuffer(nullptr, d);
    IE_Check(m_ConstantsBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_ConstantsCbMapped)));

    m_Environments.Load();
    m_RenderDevice.CreateGPUTimers(m_GpuTimingState);

    m_SceneResources.RefreshAvailableScenes();
    const CommandLineArguments& args = GetCommandLineArguments();
    const String startupSceneArg = args.sceneFile.empty() ? "Sponza" : args.sceneFile;
    String startupScene = m_SceneResources.ResolveSceneName(startupSceneArg);
    const Vector<String>& loadableScenes = m_SceneResources.GetLoadableScenes();
    if (loadableScenes.empty())
    {
        IE_LogError("No compatible scene packs were found in data/scenes. Repack scenes with a scene packer that produces .ikp version {}.", IEPack::PACK_VERSION_LATEST);
        IE_Assert(false);
    }
    else if (!m_SceneResources.CanLoadScene(startupScene))
    {
        const SceneUtils::SceneListEntry* startupSceneInfo = m_SceneResources.FindAvailableScene(startupSceneArg);
        if (startupSceneInfo != nullptr && startupSceneInfo->outOfDate)
        {
            IE_LogWarn("Startup scene '{}' is incompatible (pack version {}, expected {}), falling back to '{}'", startupSceneInfo->name, startupSceneInfo->packVersion, IEPack::PACK_VERSION_LATEST,
                       loadableScenes[0]);
        }
        else
        {
            IE_LogWarn("Startup scene '{}' not found in data/scenes, falling back to '{}'", startupSceneArg, loadableScenes[0]);
        }
        startupScene = loadableScenes[0];
    }
    ImGui_InitParams imGuiInitParams;
    imGuiInitParams.device = m_RenderDevice.GetDevice().Get();
    imGuiInitParams.queue = m_RenderDevice.GetCommandQueue().Get();
    imGuiInitParams.hwnd = m_Window.GetHwnd();
    imGuiInitParams.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ImGui_Init(imGuiInitParams);

    m_SceneResources.RequestSceneSwitch(startupScene);
    m_PendingSceneSwitchDelayFrames = 0;
    PresentLoadingFrame();

    ReloadShaders();
}

void Renderer::Terminate()
{
    WaitForGpuIdle();

    if (m_ConstantsBuffer && m_ConstantsBuffer->resource && m_ConstantsCbMapped)
    {
        m_ConstantsBuffer->Unmap(0, nullptr);
        m_ConstantsCbMapped = nullptr;
    }

    m_Sky.Terminate();
    ImGui_Shutdown();
    m_RenderDevice.Terminate();
}

void Renderer::Render()
{
    m_CpuTimers.passCount = 0;
    m_CpuTimers.openScopeCount = 0;
    m_FrameInFlightIdx = m_RenderDevice.GetCurrentBackBufferIndex();
    ProcessPendingSceneSwitch();

    PerFrameData& frameData = GetCurrentFrameData();
    ComPtr<ID3D12GraphicsCommandList7> cmd;
    Camera::FrameData cameraFrameData{};
    f32 jitterNormX = 0.f, jitterNormY = 0.f;

    CPU_MARKER_BEGIN(m_CpuTimers, "Wait for GPU");
    m_RenderDevice.WaitForFrame(frameData);
    CPU_MARKER_END(m_CpuTimers);

    CPU_MARKER_BEGIN(m_CpuTimers, "Frame Setup");
    BeginFrame(frameData, cmd, cameraFrameData, jitterNormX, jitterNormY);
    CPU_MARKER_END(m_CpuTimers);

    CPU_MARKER_BEGIN(m_CpuTimers, "Instance Motion");
    ApplyTestInstanceMotion();
    CPU_MARKER_END(m_CpuTimers);

    const bool updateInstances = m_SceneResources.AreInstancesDirty();
    const Vector<Primitive>& scenePrimitives = m_SceneResources.GetPrimitives();
    const Vector<Material>& sceneMaterials = m_SceneResources.GetMaterials();
    const Vector<InstanceData>& sceneInstances = m_SceneResources.GetInstances();
    const SharedPtr<Buffer>& materialsBuffer = m_SceneResources.GetMaterialsBuffer();

    BuildParams cullingParams{};
    cullingParams.primitives = &scenePrimitives;
    cullingParams.materials = &sceneMaterials;
    cullingParams.instances = &sceneInstances;
    cullingParams.view = &cameraFrameData.view;
    cullingParams.nearPlane = cameraFrameData.znearfar.x;
    cullingParams.frustumCullFovDeg = g_Settings.cameraFrustumCullingFov;
    cullingParams.aspectRatio = m_Window.GetAspectRatio();
    cullingParams.cpuFrustumCullingEnabled = g_Settings.cpuFrustumCulling;
    cullingParams.updateInstances = updateInstances;
    cullingParams.debugMeshletColorEnabled = g_Settings.debugMeshletColor;
    cullingParams.materialsBufferSrvIndex = materialsBuffer->srvIndex;
    cullingParams.cpuTimers = &m_CpuTimers;

    Culling& culling = m_Culling;
    culling.Build(cullingParams);

    CPU_MARKER_BEGIN(m_CpuTimers, "Ray Tracing Instance Upload");
    if (updateInstances)
    {
        const Vector<Raytracing::RTInstance>& rtInstances = culling.GetRTInstances();
        if (!rtInstances.empty())
        {
            m_Raytracing.UpdateInstances(cmd, rtInstances);
        }
        m_SceneResources.ClearInstancesDirty();
    }
    CPU_MARKER_END(m_CpuTimers);

    CPU_MARKER_BEGIN(m_CpuTimers, "Render Pass Recording");
    Pass_DepthPre(cmd);
    Pass_GBuffer(cmd);
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount> gbufferRtvs = {
        m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.rtv,   m_GBuf.gbuffers[m_FrameInFlightIdx].normal.rtv,       m_GBuf.gbuffers[m_FrameInFlightIdx].normalGeo.rtv,
        m_GBuf.gbuffers[m_FrameInFlightIdx].material.rtv, m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.rtv, m_GBuf.gbuffers[m_FrameInFlightIdx].ao.rtv,
    };
    m_Sky.PassSkyMotion(cmd, frameData.gpuTimers, m_FrameInFlightIdx, cameraFrameData, gbufferRtvs, m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector, m_DepthPre.dsvs[m_FrameInFlightIdx].srvIndex,
                        m_BindlessHeaps);
    Pass_DLSSRRGuides(cmd, cameraFrameData);
    m_Sky.PassProceduralSkyCube(cmd, frameData.gpuTimers, m_Environments.GetCurrentEnvironment(), m_BindlessHeaps);
    Pass_PathTrace(cmd);
    Raytracing::PathTracePassResources& pathTraceResources = m_Raytracing.GetPathTracePassResources();
    m_AutoExposure.Pass(cmd, frameData.gpuTimers, m_BindlessHeaps, pathTraceResources.trace.outputTexture, pathTraceResources.trace.outputTexture.srvIndex,
                        m_DepthPre.dsvs[m_FrameInFlightIdx].srvIndex, m_Upscale.renderSize, m_Window.GetFrameTimeMs());
    Pass_Upscale(cmd, cameraFrameData);
    Pass_Bloom(cmd);
    Pass_Tonemap(cmd);
    CPU_MARKER_END(m_CpuTimers);

    Timings_CollectCpu(m_CpuTimers, m_CpuTimingState);
    Timings_UpdateAverages(m_CpuTimingState, m_Window.GetFrameTimeMs(), g_Settings.timingAverageWindowMs);
    Pass_ImGui(cmd, cameraFrameData);
    Pass_PresentComposite(cmd);

    EndFrame(frameData, cmd);
}

void Renderer::MarkInstancesDirty()
{
    m_SceneResources.MarkInstancesDirty();
}

SharedPtr<Buffer> Renderer::CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc)
{
    return m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd, createDesc);
}

void Renderer::BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY)
{
    m_RenderDevice.ClearTrackedUploads(m_FrameInFlightIdx);

    if (m_PendingShaderReload)
    {
        g_Stats.shadersCompilationSuccess = true;
        ReloadShaders();
        m_PendingShaderReload = false;
    }

    Timings_CollectGpu(frameData.gpuTimers, m_GpuTimingState);
    Timings_UpdateAverages(m_GpuTimingState, m_Window.GetFrameTimeMs(), g_Settings.timingAverageWindowMs);
    frameData.gpuTimers.passCount = 0;
    frameData.gpuTimers.nextIdx = 0;

    const u32 jitterPhaseCount = GetDLSSJitterPhaseCount(m_DLSSMode);
    const TemporalJitter::Sample jitter = TemporalJitter::ComputeHaltonJitter(m_FrameIndex, m_Upscale.renderSize.x, m_Upscale.renderSize.y, jitterPhaseCount);
    const f32 jitterScale = g_Settings.dlssJitterScale;
    jitterNormX = jitter.jitterNormX * jitterScale;
    jitterNormY = jitter.jitterNormY * jitterScale;

    Camera& camera = m_Camera;
    camera.ConfigurePerspective(m_Window.GetAspectRatio(), IE_ToRadians(g_Settings.cameraFov), IE_ToRadians(g_Settings.cameraFrustumCullingFov), 0.1f, jitterNormX, jitterNormY);
    camera.BuildFrameData();

    Environment& env = m_Environments.GetCurrentEnvironment();

    const f32 sinE = IE_Sinf(g_Settings.sunElevation);
    const f32 cosE = IE_Cosf(g_Settings.sunElevation);
    XMVECTOR sun = XMVectorSet(cosE * IE_Cosf(g_Settings.sunAzimuth), sinE, cosE * IE_Sinf(g_Settings.sunAzimuth), 0.0f);
    sun = XMVector3Normalize(sun);
    XMStoreFloat3(&env.sunDir, sun);

    cameraFrameData = camera.GetFrameData();

    const f32 jitterPxX = jitterNormX * 0.5f * static_cast<f32>(m_Upscale.renderSize.x);
    const f32 jitterPxY = -jitterNormY * 0.5f * static_cast<f32>(m_Upscale.renderSize.y);

    SubmitDLSSCommonConstants(cameraFrameData, jitterPxX, jitterPxY, m_FrameIndex == 0);

    VertexConstants constants{};
    constants.cameraPos = cameraFrameData.position;
    constants.gpuFrustumCullingEnabled = g_Settings.gpuFrustumCulling ? 1u : 0u;
    constants.gpuBackfaceCullingEnabled = g_Settings.gpuBackfaceCulling ? 1u : 0u;
    constants.materialTextureMipBias = GetDLSSMaterialTextureMipBias(m_Upscale.renderSize, m_Upscale.presentSize, 1.0f);
    constants.viewProj = cameraFrameData.viewProj;
    constants.view = cameraFrameData.view;
    constants.viewProjNoJ = cameraFrameData.viewProjNoJ;
    constants.prevViewProjNoJ = cameraFrameData.prevViewProjNoJ;
    std::memcpy(constants.planes, cameraFrameData.frustumCullingPlanes, sizeof(cameraFrameData.frustumCullingPlanes));
    std::memcpy(m_ConstantsCbMapped + m_FrameInFlightIdx * m_ConstantsCbStride, &constants, sizeof(constants));

    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));
    cmd = frameData.cmd;

    cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
    cmd->RSSetViewports(1, &m_RenderViewport);
    cmd->RSSetScissorRects(1, &m_RenderRect);
}

void Renderer::Pass_DepthPre(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    m_DepthPre.dsvs[m_FrameInFlightIdx].Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cmd->OMSetRenderTargets(0, nullptr, false, &m_DepthPre.dsvs[m_FrameInFlightIdx].dsv);
    cmd->ClearDepthStencilView(m_DepthPre.dsvs[m_FrameInFlightIdx].dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    auto drawPrimitives = [&](const Vector<PrimitiveRenderData>& primitivesRenderData) {
        const Vector<Primitive>& primitives = m_SceneResources.GetPrimitives();
        for (const PrimitiveRenderData& primitiveRenderData : primitivesRenderData)
        {
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(primitiveRenderData.primConstants) / 4, &primitiveRenderData.primConstants, 0);
            cmd->DispatchMesh(IE_DivRoundUp(primitives[primitiveRenderData.primIndex].meshletCount, 32), 1, 1);
        }
    };

    PerFrameData& frameData = GetCurrentFrameData();
    const D3D12_GPU_VIRTUAL_ADDRESS frameCbGpuAddress = m_ConstantsBuffer->GetGPUVirtualAddress() + static_cast<u64>(m_FrameInFlightIdx) * m_ConstantsCbStride;
    const PrimitiveBuckets& primitiveBuckets = m_Culling.GetPrimitiveBuckets();
    const Vector<PrimitiveRenderData>& opaqueBack = primitiveBuckets[AlphaMode_Opaque][CullMode_Back];
    const Vector<PrimitiveRenderData>& opaqueNone = primitiveBuckets[AlphaMode_Opaque][CullMode_None];
    const Vector<PrimitiveRenderData>& maskedBack = primitiveBuckets[AlphaMode_Mask][CullMode_Back];
    const Vector<PrimitiveRenderData>& maskedNone = primitiveBuckets[AlphaMode_Mask][CullMode_None];

    if (!opaqueBack.empty() || !opaqueNone.empty())
    {
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Prepass - Opaque");
        {
            cmd->SetGraphicsRootSignature(m_DepthPre.opaqueRootSig.Get());
            cmd->SetGraphicsRootConstantBufferView(1, frameCbGpuAddress);
            if (!opaqueBack.empty())
            {
                cmd->SetPipelineState(m_DepthPre.opaquePSO[CullMode_Back].Get());
                drawPrimitives(opaqueBack);
            }
            if (!opaqueNone.empty())
            {
                cmd->SetPipelineState(m_DepthPre.opaquePSO[CullMode_None].Get());
                drawPrimitives(opaqueNone);
            }
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    if (!maskedBack.empty() || !maskedNone.empty())
    {
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Depth Prepass - Alpha Tested");
        {
            cmd->SetGraphicsRootSignature(m_DepthPre.alphaTestRootSig.Get());
            cmd->SetGraphicsRootConstantBufferView(1, frameCbGpuAddress);
            if (!maskedBack.empty())
            {
                cmd->SetPipelineState(m_DepthPre.alphaTestPSO[CullMode_Back].Get());
                drawPrimitives(maskedBack);
            }
            if (!maskedNone.empty())
            {
                cmd->SetPipelineState(m_DepthPre.alphaTestPSO[CullMode_None].Get());
                drawPrimitives(maskedNone);
            }
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    m_DepthPre.dsvs[m_FrameInFlightIdx].Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_READ);
}

void Renderer::Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    auto& gb = m_GBuf.gbuffers[m_FrameInFlightIdx];

    auto TransitionGBuffer = [&](D3D12_RESOURCE_STATES to) {
        gb.albedo.Transition(cmd, to);
        gb.normal.Transition(cmd, to);
        gb.normalGeo.Transition(cmd, to);
        gb.material.Transition(cmd, to);
        gb.motionVector.Transition(cmd, to);
        gb.ao.Transition(cmd, to);
        gb.emissive.Transition(cmd, to);
    };

    TransitionGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto drawPrimitives = [&](const Vector<PrimitiveRenderData>& primitivesRenderData) {
        const Vector<Primitive>& primitives = m_SceneResources.GetPrimitives();
        for (const PrimitiveRenderData& primitiveRenderData : primitivesRenderData)
        {
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(primitiveRenderData.primConstants) / 4, &primitiveRenderData.primConstants, 0);
            cmd->DispatchMesh(IE_DivRoundUp(primitives[primitiveRenderData.primIndex].meshletCount, 32), 1, 1);
        }
    };

    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
    const D3D12_GPU_VIRTUAL_ADDRESS frameCbGpuAddress = m_ConstantsBuffer->GetGPUVirtualAddress() + static_cast<u64>(m_FrameInFlightIdx) * m_ConstantsCbStride;
    const PrimitiveBuckets& primitiveBuckets = m_Culling.GetPrimitiveBuckets();
    for (AlphaMode alphaMode : {AlphaMode_Opaque, AlphaMode_Mask})
    {
        const Vector<PrimitiveRenderData>& drawBack = primitiveBuckets[alphaMode][CullMode_Back];
        const Vector<PrimitiveRenderData>& drawNone = primitiveBuckets[alphaMode][CullMode_None];
        if (drawBack.empty() && drawNone.empty())
        {
            continue;
        }

        const char* passName = alphaMode == AlphaMode_Opaque ? "GBuffer - Opaque" : "GBuffer - Alpha Tested";
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, passName);
        {
            cmd->SetGraphicsRootSignature(m_GBuf.rootSigs[alphaMode].Get());
            Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount> rtvs = {gb.albedo.rtv, gb.normal.rtv, gb.normalGeo.rtv, gb.material.rtv, gb.motionVector.rtv, gb.ao.rtv, gb.emissive.rtv};
            cmd->OMSetRenderTargets(GBuffer::targetCount, rtvs.data(), false, &m_DepthPre.dsvs[m_FrameInFlightIdx].dsv);
            cmd->SetGraphicsRootConstantBufferView(1, frameCbGpuAddress);

            if (!drawBack.empty())
            {
                cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_Back].Get());
                drawPrimitives(drawBack);
            }

            if (!drawNone.empty())
            {
                cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_None].Get());
                drawPrimitives(drawNone);
            }
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    TransitionGBuffer(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    m_DepthPre.dsvs[m_FrameInFlightIdx].Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::Pass_PathTrace(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    Raytracing& raytracing = m_Raytracing;
    const Environment& env = m_Environments.GetCurrentEnvironment();

    Raytracing::PathTracePassInput pathTracePassInput{};
    pathTracePassInput.depthTextureIndex = m_DepthPre.dsvs[m_FrameInFlightIdx].srvIndex;
    pathTracePassInput.albedoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.srvIndex;
    pathTracePassInput.normalTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.srvIndex;
    pathTracePassInput.normalGeoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normalGeo.srvIndex;
    pathTracePassInput.materialTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].material.srvIndex;
    pathTracePassInput.emissiveTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].emissive.srvIndex;
    pathTracePassInput.sunDir = env.sunDir;
    pathTracePassInput.sunColor = env.sky.sunColor;
    pathTracePassInput.sunDiskAngleDeg = env.sky.sunDiskAngleDeg;
    pathTracePassInput.frameIndex = m_FrameIndex;
    pathTracePassInput.materialsBufferIndex = m_SceneResources.GetMaterialsBuffer()->srvIndex;
    pathTracePassInput.skyCubeIndex = m_Sky.GetSkyCubeSrvIndex();
    pathTracePassInput.samplerIndex = m_SceneResources.GetLinearSamplerIdx();
    raytracing.PathTracePass(cmd, GetCurrentFrameData().gpuTimers, m_Upscale.renderSize, pathTracePassInput);
}

void Renderer::Pass_DLSSRRGuides(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    constexpr u32 kGuideDispatchGroupSize = 8u;
    static_assert(sizeof(DLSSRRGuideConstants) == 25u * sizeof(u32));

    PerFrameData& frameData = GetCurrentFrameData();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    Texture& diffuseAlbedo = m_DLSSRRGuides.diffuseAlbedo[m_FrameInFlightIdx];
    Texture& specularAlbedo = m_DLSSRRGuides.specularAlbedo[m_FrameInFlightIdx];

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "DLSS RR Guides");
    {
        diffuseAlbedo.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        specularAlbedo.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DLSSRRGuideConstants c{};
        c.invViewProj = cameraFrameData.invViewProj;
        c.cameraPos = cameraFrameData.position;
        c.albedoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.srvIndex;
        c.normalTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.srvIndex;
        c.materialTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].material.srvIndex;
        c.depthTextureIndex = m_DepthPre.dsvs[m_FrameInFlightIdx].srvIndex;
        c.diffuseAlbedoOutputTextureIndex = diffuseAlbedo.uavIndex;
        c.specularAlbedoOutputTextureIndex = specularAlbedo.uavIndex;

        cmd->SetComputeRootSignature(m_DLSSRRGuides.rootSig.Get());
        cmd->SetPipelineState(m_DLSSRRGuides.pso.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(m_Upscale.renderSize.x, kGuideDispatchGroupSize), IE_DivRoundUp(m_Upscale.renderSize.y, kGuideDispatchGroupSize), 1);

        diffuseAlbedo.UavBarrier(cmd);
        specularAlbedo.UavBarrier(cmd);

        diffuseAlbedo.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        specularAlbedo.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_Upscale(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();

    auto EnsureOutputState = [&](u32 idx, D3D12_RESOURCE_STATES desired) {
        if (m_Upscale.outputs[idx].state != desired)
        {
            m_Upscale.outputs[idx].Transition(cmd, desired);
        }
    };

    EnsureOutputState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "DLSS RR");
    {
        const Raytracing::PathTracePassResources& pathTracePassResources = m_Raytracing.GetPathTracePassResources();
        Texture& diffuseAlbedoGuide = m_DLSSRRGuides.diffuseAlbedo[m_FrameInFlightIdx];
        Texture& specularAlbedoGuide = m_DLSSRRGuides.specularAlbedo[m_FrameInFlightIdx];
        RenderTarget& normalRoughnessGuide = m_GBuf.gbuffers[m_FrameInFlightIdx].normal;
        IE_Assert(MatchesTexture(diffuseAlbedoGuide.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.renderSize));
        IE_Assert(MatchesTexture(specularAlbedoGuide.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.renderSize));
        IE_Assert(MatchesTexture(normalRoughnessGuide.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.renderSize));
        IE_Assert(MatchesTexture(pathTracePassResources.trace.hitDistanceTexture.Get(), DXGI_FORMAT_R16_FLOAT, m_Upscale.renderSize));
        IE_Assert(MatchesTexture(pathTracePassResources.trace.outputTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.renderSize));
        IE_Assert(MatchesTexture(m_Upscale.outputs[m_FrameInFlightIdx].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.presentSize));

        const D3D12_RESOURCE_DESC hdrDesc = pathTracePassResources.trace.outputTexture.GetDesc();
        const D3D12_RESOURCE_DESC outDesc = m_Upscale.outputs[m_FrameInFlightIdx].GetDesc();
        const D3D12_RESOURCE_DESC mvDesc = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.GetDesc();
        const D3D12_RESOURCE_DESC diffuseAlbedoDesc = diffuseAlbedoGuide.GetDesc();
        const D3D12_RESOURCE_DESC specularAlbedoDesc = specularAlbedoGuide.GetDesc();
        const D3D12_RESOURCE_DESC normalRoughnessDesc = normalRoughnessGuide.GetDesc();
        const D3D12_RESOURCE_DESC specularHitDistanceDesc = pathTracePassResources.trace.hitDistanceTexture.GetDesc();

        DLSS::EvaluateDesc d{};
        d.cmd = cmd.Get();
        d.colorIn = pathTracePassResources.trace.outputTexture.Get();
        d.colorOut = m_Upscale.outputs[m_FrameInFlightIdx].Get();
        d.depth = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
        d.motionVectors = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.Get();
        d.exposure = m_AutoExposure.GetFinalExposureTexture().Get();
        d.diffuseAlbedo = diffuseAlbedoGuide.Get();
        d.specularAlbedo = specularAlbedoGuide.Get();
        d.normalRoughness = normalRoughnessGuide.Get();
        d.specularHitDistance = pathTracePassResources.trace.hitDistanceTexture.Get();

        d.colorInFormat = hdrDesc.Format;
        d.colorOutFormat = outDesc.Format;
        d.depthFormat = m_DepthPre.dsvs[m_FrameInFlightIdx].GetDesc().Format;
        d.motionVectorsFormat = mvDesc.Format;
        d.exposureFormat = m_AutoExposure.GetFinalExposureTexture().GetDesc().Format;
        d.diffuseAlbedoFormat = diffuseAlbedoDesc.Format;
        d.specularAlbedoFormat = specularAlbedoDesc.Format;
        d.normalRoughnessFormat = normalRoughnessDesc.Format;
        d.specularHitDistanceFormat = specularHitDistanceDesc.Format;

        d.colorInState = pathTracePassResources.trace.outputTexture.state;
        d.colorOutState = m_Upscale.outputs[m_FrameInFlightIdx].state;
        d.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        d.motionVectorsState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        d.exposureState = m_AutoExposure.GetFinalExposureTexture().state;
        d.diffuseAlbedoState = diffuseAlbedoGuide.state;
        d.specularAlbedoState = specularAlbedoGuide.state;
        d.normalRoughnessState = normalRoughnessGuide.state;
        d.specularHitDistanceState = pathTracePassResources.trace.hitDistanceTexture.state;

        d.renderWidth = m_Upscale.renderSize.x;
        d.renderHeight = m_Upscale.renderSize.y;
        d.outputWidth = m_Upscale.presentSize.x;
        d.outputHeight = m_Upscale.presentSize.y;
        d.streamlineFrameIndex = m_StreamlineFrameIndex;
        d.mode = m_DLSSMode;
        d.invView = cameraFrameData.invView;

        DLSS::Evaluate(d);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    m_Upscale.outputs[m_FrameInFlightIdx].UavBarrier(cmd);
    EnsureOutputState(m_FrameInFlightIdx, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

void Renderer::Pass_Bloom(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    if (!g_Settings.bloomEnabled || g_Settings.bloomIntensity <= 0.0f)
    {
        return;
    }

    PerFrameData& frameData = GetCurrentFrameData();
    GpuResource* source = &m_Upscale.outputs[m_FrameInFlightIdx];
    u32 sourceSrvIndex = m_Upscale.outputs[m_FrameInFlightIdx].srvIndex;

    source->Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    const u32 linearSamplerIndex = m_SceneResources.GetLinearSamplerIdx();
    auto dispatchForMip = [&](u32 mip) { return XMUINT2(IE_DivRoundUp(m_Bloom.mipSizes[mip].x, 8u), IE_DivRoundUp(m_Bloom.mipSizes[mip].y, 8u)); };

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Bloom");
    {
        cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());

        cmd->SetComputeRootSignature(m_Bloom.downsampleRootSig.Get());
        cmd->SetPipelineState(m_Bloom.downsamplePso.Get());
        for (u32 mip = 0; mip < m_Bloom.mipCount; ++mip)
        {
            Texture& dst = m_Bloom.downChain[m_FrameInFlightIdx][mip];

            BloomDownsampleConstants c{};
            c.inputTextureIndex = (mip == 0u) ? sourceSrvIndex : m_Bloom.downChain[m_FrameInFlightIdx][mip - 1u].srvIndex;
            c.outputTextureIndex = dst.uavIndex;
            c.samplerIndex = linearSamplerIndex;
            c.applyThreshold = (mip == 0u) ? 1u : 0u;
            c.threshold = g_Settings.bloomThreshold;
            c.softKnee = g_Settings.bloomSoftKnee;

            const XMUINT2 dispatch = dispatchForMip(mip);
            dst.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
            cmd->Dispatch(dispatch.x, dispatch.y, 1);
            dst.UavBarrier(cmd);
            dst.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }

        if (m_Bloom.mipCount > 1u)
        {
            cmd->SetComputeRootSignature(m_Bloom.upsampleRootSig.Get());
            cmd->SetPipelineState(m_Bloom.upsamplePso.Get());
            for (i32 mip = static_cast<i32>(m_Bloom.mipCount) - 2; mip >= 0; --mip)
            {
                Texture& dst = m_Bloom.upChain[m_FrameInFlightIdx][mip];

                BloomUpsampleConstants c{};
                c.baseTextureIndex = m_Bloom.downChain[m_FrameInFlightIdx][mip].srvIndex;
                c.bloomTextureIndex = (mip == static_cast<i32>(m_Bloom.mipCount) - 2) ? m_Bloom.downChain[m_FrameInFlightIdx][mip + 1].srvIndex : m_Bloom.upChain[m_FrameInFlightIdx][mip + 1].srvIndex;
                c.outputTextureIndex = dst.uavIndex;
                c.samplerIndex = linearSamplerIndex;

                const XMUINT2 dispatch = dispatchForMip(static_cast<u32>(mip));
                dst.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
                cmd->Dispatch(dispatch.x, dispatch.y, 1);
                dst.UavBarrier(cmd);
                dst.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_Tonemap(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Tone Mapping");
    {
        m_Tonemap.sdrRt[m_FrameInFlightIdx].Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

        TonemapConstants t{};
        t.srvIndex = m_Upscale.outputs[m_FrameInFlightIdx].srvIndex;
        t.samplerIndex = m_SceneResources.GetLinearSamplerIdx();
        t.bloomTextureIndex = (m_Bloom.mipCount > 1u) ? m_Bloom.upChain[m_FrameInFlightIdx][0].srvIndex : m_Bloom.downChain[m_FrameInFlightIdx][0].srvIndex;
        t.exposureTextureIndex = m_AutoExposure.GetFinalExposureTextureSrvIndex();
        t.contrast = g_Settings.toneMappingContrast;
        t.saturation = g_Settings.toneMappingSaturation;
        t.bloomIntensity = g_Settings.bloomEnabled ? g_Settings.bloomIntensity : 0.0f;

        cmd->RSSetViewports(1, &m_PresentViewport);
        cmd->RSSetScissorRects(1, &m_PresentRect);
        cmd->OMSetRenderTargets(1, &m_Tonemap.sdrRt[m_FrameInFlightIdx].rtv, false, nullptr);
        cmd->SetPipelineState(m_Tonemap.pso.Get());
        cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
        cmd->SetGraphicsRootSignature(m_Tonemap.rootSig.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(t) / 4, &t, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_ImGui(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "UI");
    {
        RenderTarget& uiRt = m_Tonemap.uiRt[m_FrameInFlightIdx];
        uiRt.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd->ClearRenderTargetView(uiRt.rtv, clearColor, 0, nullptr);
        cmd->RSSetViewports(1, &m_PresentViewport);
        cmd->RSSetScissorRects(1, &m_PresentRect);

        static ImGui_TimingValue gpuTimings[128];
        static ImGui_TimingValue cpuTimings[128];

        const f32 timingWindowMs = g_Settings.timingAverageWindowMs;

        u32 nGpuTimings = m_GpuTimingState.lastCount;
        for (u32 i = 0; i < nGpuTimings; ++i)
        {
            gpuTimings[i].name = m_GpuTimingState.last[i].name;
            gpuTimings[i].ms = Timings_ComputeAverageWindowMs(m_GpuTimingState, gpuTimings[i].name, timingWindowMs);
        }

        u32 nCpuTimings = m_CpuTimingState.lastCount;
        for (u32 i = 0; i < nCpuTimings; ++i)
        {
            cpuTimings[i].name = m_CpuTimingState.last[i].name;
            cpuTimings[i].ms = Timings_ComputeAverageWindowMs(m_CpuTimingState, cpuTimings[i].name, timingWindowMs);
        }

        ImGui_FrameStats frameStats{};
        frameStats.fps = static_cast<u32>(m_Window.GetFPS());
        frameStats.cameraPos[0] = cameraFrameData.position.x;
        frameStats.cameraPos[1] = cameraFrameData.position.y;
        frameStats.cameraPos[2] = cameraFrameData.position.z;
        frameStats.cameraYaw = cameraFrameData.yaw;
        frameStats.cameraPitch = cameraFrameData.pitch;

        const Raytracing::PathTracePassResources& pathTracePassResources = m_Raytracing.GetPathTracePassResources();
        Texture& rrDiffuseAlbedo = m_DLSSRRGuides.diffuseAlbedo[m_FrameInFlightIdx];
        Texture& rrSpecularAlbedo = m_DLSSRRGuides.specularAlbedo[m_FrameInFlightIdx];

        ImGui_RenderParams rp{};
        rp.cmd = cmd.Get();
        rp.renderer = this;
        rp.rtv = uiRt.rtv;
        rp.gbufferAlbedo = m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.Get();
        rp.gbufferNormal = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.Get();
        rp.gbufferNormalGeo = m_GBuf.gbuffers[m_FrameInFlightIdx].normalGeo.Get();
        rp.gbufferMaterial = m_GBuf.gbuffers[m_FrameInFlightIdx].material.Get();
        rp.gbufferMotion = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.Get();
        rp.gbufferAO = m_GBuf.gbuffers[m_FrameInFlightIdx].ao.Get();
        rp.gbufferEmissive = m_GBuf.gbuffers[m_FrameInFlightIdx].emissive.Get();
        rp.depth = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
        rp.pathTrace = pathTracePassResources.trace.outputTexture.Get();
        rp.dlssRRDiffuseAlbedo = rrDiffuseAlbedo.Get();
        rp.dlssRRSpecularAlbedo = rrSpecularAlbedo.Get();
        rp.dlssRRNormalRoughness = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.Get();
        rp.dlssRRSpecularHitDistance = pathTracePassResources.trace.hitDistanceTexture.Get();
        rp.dlssRROutput = m_Upscale.outputs[m_FrameInFlightIdx].Get();
        rp.frame = frameStats;
        rp.gpuTimings = gpuTimings;
        rp.gpuTimingsCount = nGpuTimings;
        rp.cpuTimings = cpuTimings;
        rp.cpuTimingsCount = nCpuTimings;
        ImGui_Render(rp);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::Pass_PresentComposite(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Present Composite");
    {
        RenderTarget& hudlessRt = m_Tonemap.sdrRt[m_FrameInFlightIdx];
        RenderTarget& uiRt = m_Tonemap.uiRt[m_FrameInFlightIdx];
        RenderTarget& backBufferRt = m_Tonemap.backBufferRt[m_FrameInFlightIdx];

        hudlessRt.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        uiRt.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        backBufferRt.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

        PresentCompositeConstants constants{};
        constants.hudlessTextureIndex = hudlessRt.srvIndex;
        constants.uiTextureIndex = uiRt.srvIndex;
        constants.samplerIndex = m_SceneResources.GetLinearSamplerIdx();

        cmd->RSSetViewports(1, &m_PresentViewport);
        cmd->RSSetScissorRects(1, &m_PresentRect);
        cmd->OMSetRenderTargets(1, &backBufferRt.rtv, false, nullptr);
        cmd->SetPipelineState(m_Tonemap.composePso.Get());
        cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
        cmd->SetGraphicsRootSignature(m_Tonemap.composeRootSig.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Renderer::EndFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    if (frameData.gpuTimers.nextIdx)
    {
        cmd->ResolveQueryData(frameData.gpuTimers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, frameData.gpuTimers.nextIdx, frameData.gpuTimers.readback.Get(), 0);
    }

    const GBuffer& gbuffer = m_GBuf.gbuffers[m_FrameInFlightIdx];
    DLSS::FrameGenerationTagDesc tagDesc{};
    tagDesc.cmd = cmd.Get();
    tagDesc.streamlineFrameIndex = m_StreamlineFrameIndex;
    tagDesc.valid = m_FrameGenerationEnabled;
    tagDesc.depth = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
    tagDesc.motionVectors = gbuffer.motionVector.Get();
    tagDesc.hudlessColor = m_Tonemap.sdrRt[m_FrameInFlightIdx].Get();
    tagDesc.uiColorAndAlpha = m_Tonemap.uiRt[m_FrameInFlightIdx].Get();
    tagDesc.depthFormat = m_DepthPre.dsvs[m_FrameInFlightIdx].GetDesc().Format;
    tagDesc.motionVectorsFormat = gbuffer.motionVector.GetDesc().Format;
    tagDesc.hudlessFormat = m_Tonemap.sdrRt[m_FrameInFlightIdx].GetDesc().Format;
    tagDesc.uiFormat = m_Tonemap.uiRt[m_FrameInFlightIdx].GetDesc().Format;
    tagDesc.depthState = m_DepthPre.dsvs[m_FrameInFlightIdx].state;
    tagDesc.motionVectorsState = gbuffer.motionVector.state;
    tagDesc.hudlessState = m_Tonemap.sdrRt[m_FrameInFlightIdx].state;
    tagDesc.uiState = m_Tonemap.uiRt[m_FrameInFlightIdx].state;
    tagDesc.renderWidth = m_Upscale.renderSize.x;
    tagDesc.renderHeight = m_Upscale.renderSize.y;
    tagDesc.outputWidth = m_Upscale.presentSize.x;
    tagDesc.outputHeight = m_Upscale.presentSize.y;
    DLSS::TagFrameGenerationInputs(tagDesc);

    DLSS::FrameGenerationOptionsDesc optionsDesc{};
    optionsDesc.enabled = m_FrameGenerationEnabled;
    optionsDesc.renderWidth = m_Upscale.renderSize.x;
    optionsDesc.renderHeight = m_Upscale.renderSize.y;
    optionsDesc.outputWidth = m_Upscale.presentSize.x;
    optionsDesc.outputHeight = m_Upscale.presentSize.y;
    optionsDesc.backBufferFormat = m_Tonemap.backBufferRt[m_FrameInFlightIdx].GetDesc().Format;
    optionsDesc.depthFormat = m_DepthPre.dsvs[m_FrameInFlightIdx].GetDesc().Format;
    optionsDesc.motionVectorsFormat = gbuffer.motionVector.GetDesc().Format;
    optionsDesc.hudlessFormat = m_Tonemap.sdrRt[m_FrameInFlightIdx].GetDesc().Format;
    optionsDesc.uiFormat = m_Tonemap.uiRt[m_FrameInFlightIdx].GetDesc().Format;
    DLSS::SetFrameGenerationOptions(optionsDesc);

    if (m_FrameGenerationEnabled)
    {
        DLSS::FrameGenerationState fgState{};
        DLSS::GetFrameGenerationState(fgState);
        if (fgState.status != 0u)
        {
            IE_LogError("DLSS Frame Generation entered an invalid runtime state: {}", fgState.status);
            IE_Assert(false);
        }
    }

    m_Tonemap.backBufferRt[m_FrameInFlightIdx].Transition(cmd, D3D12_RESOURCE_STATE_PRESENT);

    m_RenderDevice.ExecuteFrame(frameData, cmd, m_StreamlineFrameIndex);
    CheckFrameGenerationApiError();
    m_StreamlineFrameIndex++;
    m_FrameIndex++;
}

void Renderer::CheckFrameGenerationApiError()
{
    i32 fgApiError = 0;
    if (!DLSS::ConsumeFrameGenerationApiError(fgApiError))
    {
        return;
    }

    IE_LogError("DLSS Frame Generation present-thread API error.");
    IE_Check(static_cast<HRESULT>(fgApiError));
}

void Renderer::WaitForGpuIdle()
{
    m_RenderDevice.WaitForGpuIdle();
}

void Renderer::ReloadShaders()
{
    WaitForGpuIdle();

    IE_LogInfo("Reloading shaders...");
    Shader::ResetReloadStats();

    Vector<String> globalDefines;
    CreateDepthPrePassPipelines(globalDefines);
    CreateGBufferPassPipelines(globalDefines);
    CreateDLSSRRGuidePassPipelines(globalDefines);
    m_Sky.CreateProceduralSkyCubePipelines(m_RenderDevice.GetDevice(), globalDefines);
    m_AutoExposure.CreatePipelines(m_RenderDevice.GetDevice(), globalDefines);
    m_Sky.CreateSkyMotionPassPipelines(m_RenderDevice.GetDevice(), globalDefines);
    CreateBloomPassPipelines(globalDefines);
    CreateToneMapPassPipelines(globalDefines);

    m_Raytracing.ReloadShaders();
    m_Sky.MarkProceduralSkyDirty();

    const Shader::ReloadStats reloadStats = Shader::GetReloadStats();
    IE_LogInfo("Shader reload done. Reloaded {}/{} (skipped {}, failed {}).", reloadStats.reloaded, reloadStats.total, reloadStats.skipped, reloadStats.failed);
}

void Renderer::CreateRTVs()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    const ComPtr<IDXGISwapChain4>& swapchain = m_RenderDevice.GetSwapchain();

    D3D12_DESCRIPTOR_HEAP_DESC backBufferRtvHeapDesc{};
    backBufferRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    backBufferRtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
    IE_Check(device->CreateDescriptorHeap(&backBufferRtvHeapDesc, IID_PPV_ARGS(&m_Tonemap.backBufferRtvHeap)));
    IE_Check(m_Tonemap.backBufferRtvHeap->SetName(L"Back Buffer : Heap"));

    D3D12_RENDER_TARGET_VIEW_DESC backBufferRtvDesc{};
    backBufferRtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    backBufferRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(swapchain->GetBuffer(i, IID_PPV_ARGS(&m_Tonemap.backBufferRt[i].resource)));
        m_Tonemap.backBufferRt[i].state = D3D12_RESOURCE_STATE_PRESENT;
        m_Tonemap.backBufferRt[i].SetName(L"Back Buffer");

        m_Tonemap.backBufferRt[i].rtv = m_Tonemap.backBufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_Tonemap.backBufferRt[i].rtv.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(m_Tonemap.backBufferRt[i].Get(), &backBufferRtvDesc, m_Tonemap.backBufferRt[i].rtv);
    }

    auto createPresentSizedRenderTargets = [&](Array<RenderTarget, IE_Constants::frameInFlightCount>& targets, ComPtr<ID3D12DescriptorHeap>& rtvHeap, const wchar_t* heapName,
                                               const wchar_t* resourceName, const FLOAT clearColor[4]) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
        IE_Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));
        IE_Check(rtvHeap->SetName(heapName));

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, m_Upscale.presentSize.x, m_Upscale.presentSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CLEAR_VALUE renderTargetClearValue{};
        renderTargetClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::memcpy(renderTargetClearValue.Color, clearColor, sizeof(renderTargetClearValue.Color));

        for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
        {
            ReleaseSrv(m_BindlessHeaps, targets[i].srvIndex);

            IE_Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &renderTargetClearValue,
                                                     IID_PPV_ARGS(&targets[i].resource)));
            targets[i].state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            targets[i].SetName(resourceName);

            targets[i].rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            targets[i].rtv.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            device->CreateRenderTargetView(targets[i].Get(), &rtvDesc, targets[i].rtv);
            targets[i].srvIndex = m_BindlessHeaps.CreateSRV(targets[i].resource, srvDesc);
        }
    };

    constexpr FLOAT hudlessClearColor[4] = {0.06f, 0.07f, 0.08f, 1.0f};
    createPresentSizedRenderTargets(m_Tonemap.sdrRt, m_Tonemap.rtvHeap, L"Hudless Color Target : Heap", L"Hudless Color Target", hudlessClearColor);

    constexpr FLOAT uiClearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    createPresentSizedRenderTargets(m_Tonemap.uiRt, m_Tonemap.uiRtvHeap, L"UI Color Target : Heap", L"UI Color Target", uiClearColor);
}

void Renderer::CreateDSV()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    const ComPtr<D3D12MA::Allocator>& allocator = m_RenderDevice.GetAllocator();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorDsvHeapDesc{};
    descriptorDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    descriptorDsvHeapDesc.NumDescriptors = IE_Constants::frameInFlightCount;
    IE_Check(device->CreateDescriptorHeap(&descriptorDsvHeapDesc, IID_PPV_ARGS(&m_DepthPre.dsvHeap)));
    IE_Check(m_DepthPre.dsvHeap->SetName(L"Depth/Stencil : Heap"));
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 0.0f;

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, m_Upscale.renderSize.x, m_Upscale.renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        IE_Check(allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                                           m_DepthPre.dsvs[i].allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS(&m_DepthPre.dsvs[i].resource)));
        m_DepthPre.dsvs[i].state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_DepthPre.dsvs[i].SetName(L"Depth/Stencil : DSV");

        m_DepthPre.dsvs[i].dsv = m_DepthPre.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_DepthPre.dsvs[i].dsv.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        device->CreateDepthStencilView(m_DepthPre.dsvs[i].Get(), &dsvDesc, m_DepthPre.dsvs[i].dsv);
    }
}

void Renderer::SetRenderAndPresentSize()
{
    m_Upscale.presentSize = m_Window.GetResolution();

    DLSS::OptimalSettings dlssSettings{};
    DLSS::GetOptimalSettings(m_Upscale.presentSize.x, m_Upscale.presentSize.y, m_DLSSMode, dlssSettings);
    m_Upscale.renderSize.x = dlssSettings.renderWidth;
    m_Upscale.renderSize.y = dlssSettings.renderHeight;

    m_PresentViewport.TopLeftX = 0.f;
    m_PresentViewport.TopLeftY = 0.f;
    m_PresentViewport.Width = static_cast<f32>(m_Upscale.presentSize.x);
    m_PresentViewport.Height = static_cast<f32>(m_Upscale.presentSize.y);
    m_PresentViewport.MinDepth = 0.f;
    m_PresentViewport.MaxDepth = 1.f;

    m_PresentRect.left = 0;
    m_PresentRect.top = 0;
    m_PresentRect.right = static_cast<i32>(m_Upscale.presentSize.x);
    m_PresentRect.bottom = static_cast<i32>(m_Upscale.presentSize.y);

    m_RenderViewport.TopLeftX = 0.f;
    m_RenderViewport.TopLeftY = 0.f;
    m_RenderViewport.Width = static_cast<f32>(m_Upscale.renderSize.x);
    m_RenderViewport.Height = static_cast<f32>(m_Upscale.renderSize.y);
    m_RenderViewport.MinDepth = 0.f;
    m_RenderViewport.MaxDepth = 1.f;

    m_RenderRect.left = 0;
    m_RenderRect.top = 0;
    m_RenderRect.right = static_cast<i32>(m_Upscale.renderSize.x);
    m_RenderRect.bottom = static_cast<i32>(m_Upscale.renderSize.y);
}

void Renderer::CreateUpscaleResources()
{
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        ReleaseSrv(m_BindlessHeaps, m_Upscale.outputs[i].srvIndex);

        CD3DX12_RESOURCE_DESC desc =
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_Upscale.presentSize.x, m_Upscale.presentSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        IE_Check(
            m_RenderDevice.GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_Upscale.outputs[i].resource)));
        m_Upscale.outputs[i].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_Upscale.outputs[i].SetName(L"Upscale Output");

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Upscale.outputs[i].srvIndex = m_BindlessHeaps.CreateSRV(m_Upscale.outputs[i].Get(), srvDesc);
    }
}

void Renderer::CreateBloomResources()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        for (u32 mip = 0; mip < BloomResources::kMaxMipCount; ++mip)
        {
            ReleaseTextureDescriptors(m_BindlessHeaps, m_Bloom.downChain[i][mip]);
            ReleaseTextureDescriptors(m_BindlessHeaps, m_Bloom.upChain[i][mip]);
        }
    }

    m_Bloom.mipCount = 0;
    u32 width = (m_Upscale.presentSize.x > 1u) ? (m_Upscale.presentSize.x / 2u) : 1u;
    u32 height = (m_Upscale.presentSize.y > 1u) ? (m_Upscale.presentSize.y / 2u) : 1u;
    for (u32 mip = 0; mip < BloomResources::kMaxMipCount; ++mip)
    {
        m_Bloom.mipSizes[mip] = XMUINT2(width, height);
        m_Bloom.mipCount = mip + 1u;
        if (width == 1u && height == 1u)
        {
            break;
        }
        width = (width > 1u) ? (width / 2u) : 1u;
        height = (height > 1u) ? (height / 2u) : 1u;
    }

    auto createTexture = [&](Texture& texture, const XMUINT2& size, const wchar_t* name) {
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, size.x, size.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        IE_Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture.resource)));
        texture.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        texture.SetName(name);
        texture.srvIndex = m_BindlessHeaps.CreateSRV(texture.Get(), srvDesc);
        texture.uavIndex = m_BindlessHeaps.CreateUAV(texture.Get(), uavDesc);
    };

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        for (u32 mip = 0; mip < m_Bloom.mipCount; ++mip)
        {
            createTexture(m_Bloom.downChain[i][mip], m_Bloom.mipSizes[mip], L"Bloom Down");
            createTexture(m_Bloom.upChain[i][mip], m_Bloom.mipSizes[mip], L"Bloom Up");
        }
    }
}

void Renderer::CreateDepthPrePassPipelines(const Vector<String>& globalDefines)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    Shader::ReloadOrCreate(m_GBuf.amplificationShader, IE_SHADER_TYPE_AMPLIFICATION, "systems/gbuffer/gbuffer.as.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_DepthPre.opaqueMeshShader, IE_SHADER_TYPE_MESH, "systems/depth/depth_pre_opaque.ms.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_DepthPre.alphaTestMeshShader, IE_SHADER_TYPE_MESH, "systems/depth/depth_pre_alpha_test.ms.hlsl", globalDefines);

    CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
    ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    {
        m_DepthPre.opaqueRootSig = m_DepthPre.opaqueMeshShader->GetOrCreateRootSignature(device);

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.opaqueRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader->GetBytecode();
        depthDesc.MS = m_DepthPre.opaqueMeshShader->GetBytecode();
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPre.opaquePSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPre.opaquePSO[CullMode_None])));
    }

    {
        Shader::ReloadOrCreate(m_DepthPre.alphaTestShader, IE_SHADER_TYPE_PIXEL, "systems/gbuffer/gbuffer_alpha_test.ps.hlsl", globalDefines);

        m_DepthPre.alphaTestRootSig = m_DepthPre.alphaTestShader->GetOrCreateRootSignature(device);

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.alphaTestRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader->GetBytecode();
        depthDesc.MS = m_DepthPre.alphaTestMeshShader->GetBytecode();
        depthDesc.PS = m_DepthPre.alphaTestShader->GetBytecode();
        depthDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        depthDesc.SampleMask = UINT_MAX;
        depthDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        depthDesc.DepthStencilState = ds;
        depthDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        depthDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = DefaultSampleDesc();

        CD3DX12_PIPELINE_MESH_STATE_STREAM depthStream(depthDesc);
        D3D12_PIPELINE_STATE_STREAM_DESC depthStreamDesc{sizeof(depthStream), &depthStream};
        IE_Check(device->CreatePipelineState(&depthStreamDesc, IID_PPV_ARGS(&m_DepthPre.alphaTestPSO[CullMode_Back])));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDescNoCull = depthDesc;
        auto rastNoCull = depthDescNoCull.RasterizerState;
        rastNoCull.CullMode = D3D12_CULL_MODE_NONE;
        depthDescNoCull.RasterizerState = rastNoCull;

        CD3DX12_PIPELINE_MESH_STATE_STREAM streamNoCull(depthDescNoCull);
        D3D12_PIPELINE_STATE_STREAM_DESC streamDescNoCull{sizeof(streamNoCull), &streamNoCull};
        IE_Check(device->CreatePipelineState(&streamDescNoCull, IID_PPV_ARGS(&m_DepthPre.alphaTestPSO[CullMode_None])));
    }
}

constexpr DXGI_FORMAT formats[GBuffer::targetCount] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,     // Albedo
    DXGI_FORMAT_R16G16B16A16_FLOAT, // Normal + Roughness
    DXGI_FORMAT_R16G16_SNORM,       // NormalGeo
    DXGI_FORMAT_R16_UNORM,          // Metallic
    DXGI_FORMAT_R16G16_FLOAT,       // Motion vector
    DXGI_FORMAT_R8_UNORM,           // AO
    DXGI_FORMAT_R11G11B10_FLOAT,    // Emissive
};
constexpr const wchar_t* rtvNames[GBuffer::targetCount] = {L"GBuffer Albedo", L"GBuffer Normal Roughness", L"GBuffer Normal Geometry", L"GBuffer Metallic", L"GBuffer Motion Vector",
                                                           L"GBuffer AO",     L"GBuffer Emissive"};

void Renderer::CreateGBufferPassPipelines(const Vector<String>& globalDefines)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    Shader::ReloadOrCreate(m_GBuf.amplificationShader, IE_SHADER_TYPE_AMPLIFICATION, "systems/gbuffer/gbuffer.as.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_GBuf.meshShader, IE_SHADER_TYPE_MESH, "systems/gbuffer/gbuffer.ms.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_GBuf.pixelShaders[AlphaMode_Opaque], IE_SHADER_TYPE_PIXEL, "systems/gbuffer/gbuffer.ps.hlsl", globalDefines);

    Vector<String> blendDefines = globalDefines;
    blendDefines.push_back("ENABLE_BLEND");
    Shader::ReloadOrCreate(m_GBuf.pixelShaders[AlphaMode_Blend], IE_SHADER_TYPE_PIXEL, "systems/gbuffer/gbuffer.ps.hlsl", blendDefines);

    Vector<String> maskDefines = globalDefines;
    maskDefines.push_back("ENABLE_ALPHA_TEST");
    Shader::ReloadOrCreate(m_GBuf.pixelShaders[AlphaMode_Mask], IE_SHADER_TYPE_PIXEL, "systems/gbuffer/gbuffer.ps.hlsl", maskDefines);

    for (AlphaMode alphaMode = AlphaMode_Opaque; alphaMode < AlphaMode_Count; alphaMode = static_cast<AlphaMode>(alphaMode + 1))
    {
        m_GBuf.rootSigs[alphaMode] = m_GBuf.meshShader->GetOrCreateRootSignature(device);

        CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
        ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC msDesc{};
        msDesc.pRootSignature = m_GBuf.rootSigs[alphaMode].Get();
        msDesc.AS = m_GBuf.amplificationShader->GetBytecode();
        msDesc.MS = m_GBuf.meshShader->GetBytecode();
        msDesc.PS = m_GBuf.pixelShaders[alphaMode]->GetBytecode();
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

        {
            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(msDesc);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBuf.psos[alphaMode][CullMode_Back])));
        }

        {
            D3DX12_MESH_SHADER_PIPELINE_STATE_DESC noCull = msDesc;
            D3D12_RASTERIZER_DESC rast = noCull.RasterizerState;
            rast.CullMode = D3D12_CULL_MODE_NONE;
            noCull.RasterizerState = rast;

            CD3DX12_PIPELINE_MESH_STATE_STREAM stream(noCull);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            IE_Check(device->CreatePipelineState(&desc, IID_PPV_ARGS(&m_GBuf.psos[alphaMode][CullMode_None])));
        }
    }
}

void Renderer::CreateDLSSRRGuidePassPipelines(const Vector<String>& globalDefines)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    Shader::ReloadOrCreate(m_DLSSRRGuides.cs, IE_SHADER_TYPE_COMPUTE, "systems/dlss/dlss_rr_guides.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_DLSSRRGuides.cs, m_DLSSRRGuides.rootSig, m_DLSSRRGuides.pso);
}

void Renderer::CreateBloomPassPipelines(const Vector<String>& globalDefines)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    Shader::ReloadOrCreate(m_Bloom.downsampleCs, IE_SHADER_TYPE_COMPUTE, "systems/post/bloom_extract.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Bloom.downsampleCs, m_Bloom.downsampleRootSig, m_Bloom.downsamplePso);

    Shader::ReloadOrCreate(m_Bloom.upsampleCs, IE_SHADER_TYPE_COMPUTE, "systems/post/bloom_blur.cs.hlsl", globalDefines);
    PipelineHelpers::CreateComputePipeline(device, m_Bloom.upsampleCs, m_Bloom.upsampleRootSig, m_Bloom.upsamplePso);
}

void Renderer::CreateToneMapPassPipelines(const Vector<String>& globalDefines)
{
    Shader::ReloadOrCreate(m_Tonemap.vxShader, IE_SHADER_TYPE_VERTEX, "shared/fullscreen.vs.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_Tonemap.pxShader, IE_SHADER_TYPE_PIXEL, "systems/post/tonemap.ps.hlsl", globalDefines);
    Shader::ReloadOrCreate(m_Tonemap.composePxShader, IE_SHADER_TYPE_PIXEL, "systems/post/present_composite.ps.hlsl", globalDefines);
    PipelineHelpers::CreateFullscreenGraphicsPipeline(m_RenderDevice.GetDevice(), m_Tonemap.vxShader, m_Tonemap.pxShader, DXGI_FORMAT_R8G8B8A8_UNORM, m_Tonemap.rootSig, m_Tonemap.pso);
    PipelineHelpers::CreateFullscreenGraphicsPipeline(m_RenderDevice.GetDevice(), m_Tonemap.vxShader, m_Tonemap.composePxShader, DXGI_FORMAT_R8G8B8A8_UNORM, m_Tonemap.composeRootSig,
                                                      m_Tonemap.composePso);
}

void Renderer::CreateGBufferPassResources()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv2D{};
    srv2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv2D.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv2D.Texture2D.MipLevels = 1;

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        GBuffer& gbuff = m_GBuf.gbuffers[i];
        RenderTarget* targets[GBuffer::targetCount] = {&gbuff.albedo, &gbuff.normal, &gbuff.normalGeo, &gbuff.material, &gbuff.motionVector, &gbuff.ao, &gbuff.emissive};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = GBuffer::targetCount;
        IE_Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gbuff.rtvHeap)));

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gbuff.rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (u32 t = 0; t < GBuffer::targetCount; ++t)
        {
            ReleaseSrv(m_BindlessHeaps, targets[t]->srvIndex);

            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[t], m_Upscale.renderSize.x, m_Upscale.renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE clearValue = {formats[t], {0.f, 0.f, 0.f, 0.f}};
            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

            IE_Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&targets[t]->resource)));
            targets[t]->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            targets[t]->SetName(rtvNames[t]);

            targets[t]->rtv = rtvHandle;
            device->CreateRenderTargetView(targets[t]->Get(), nullptr, targets[t]->rtv);

            srv2D.Format = targets[t]->GetDesc().Format;
            targets[t]->srvIndex = m_BindlessHeaps.CreateSRV(targets[t]->resource, srv2D);

            rtvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
}

void Renderer::CreateDLSSRRGuideResources()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    const XMUINT2 size = m_Upscale.renderSize;

    auto createGuideTexture = [&](Texture& texture, DXGI_FORMAT format, const wchar_t* name) {
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, size.x, size.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        IE_Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture.resource)));
        texture.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        texture.SetName(name);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        texture.srvIndex = m_BindlessHeaps.CreateSRV(texture.resource, srvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        texture.uavIndex = m_BindlessHeaps.CreateUAV(texture.resource, uavDesc);
    };

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        ReleaseTextureDescriptors(m_BindlessHeaps, m_DLSSRRGuides.diffuseAlbedo[i]);
        ReleaseTextureDescriptors(m_BindlessHeaps, m_DLSSRRGuides.specularAlbedo[i]);
        createGuideTexture(m_DLSSRRGuides.diffuseAlbedo[i], DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS RR Diffuse Albedo");
        createGuideTexture(m_DLSSRRGuides.specularAlbedo[i], DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS RR Specular Albedo");
    }
}

void Renderer::InvalidateRuntimeDescriptorIndices()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        m_DepthPre.dsvs[i].srvIndex = UINT32_MAX;

        m_GBuf.gbuffers[i].albedo.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].normal.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].normalGeo.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].material.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].motionVector.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].ao.srvIndex = UINT32_MAX;
        m_GBuf.gbuffers[i].emissive.srvIndex = UINT32_MAX;

        m_DLSSRRGuides.diffuseAlbedo[i].srvIndex = UINT32_MAX;
        m_DLSSRRGuides.diffuseAlbedo[i].uavIndex = UINT32_MAX;
        m_DLSSRRGuides.specularAlbedo[i].srvIndex = UINT32_MAX;
        m_DLSSRRGuides.specularAlbedo[i].uavIndex = UINT32_MAX;

        m_Upscale.outputs[i].srvIndex = UINT32_MAX;
        m_Upscale.outputs[i].uavIndex = UINT32_MAX;

        m_Tonemap.sdrRt[i].srvIndex = UINT32_MAX;
        m_Tonemap.uiRt[i].srvIndex = UINT32_MAX;

        for (u32 mip = 0; mip < BloomResources::kMaxMipCount; ++mip)
        {
            m_Bloom.downChain[i][mip].srvIndex = UINT32_MAX;
            m_Bloom.downChain[i][mip].uavIndex = UINT32_MAX;
            m_Bloom.upChain[i][mip].srvIndex = UINT32_MAX;
            m_Bloom.upChain[i][mip].uavIndex = UINT32_MAX;
        }
    }

    m_AutoExposure.InvalidateDescriptorIndices();
    m_Raytracing.InvalidatePathTraceDescriptorIndices();
}

void Renderer::PrepareRuntimeReload()
{
    WaitForGpuIdle();
    ImGui_ResetDebugTextureCache();

    Timings_ResetAverages(m_GpuTimingState);
    Timings_ResetAverages(m_CpuTimingState);
    for (PerFrameData& frameData : m_RenderDevice.GetAllFrameData())
    {
        frameData.gpuTimers.passCount = 0;
        frameData.gpuTimers.nextIdx = 0;
    }
}

void Renderer::RecreateRuntimeFrameResources(const bool recreateSkyCube)
{
    SetRenderAndPresentSize();
    CreateRTVs();
    CreateDSV();
    CreateSceneDepthSRVs();
    CreateUpscaleResources();
    CreateBloomResources();
    CreateGBufferPassResources();
    CreateDLSSRRGuideResources();
    if (recreateSkyCube)
    {
        m_Sky.CreateProceduralSkyCubeResources(m_RenderDevice.GetDevice(), m_BindlessHeaps);
    }
    m_AutoExposure.CreateResources(m_RenderDevice, m_BindlessHeaps);
    m_Sky.CreateSkyMotionPassResources(m_RenderDevice.GetDevice());
}

void Renderer::SubmitDLSSCommonConstants(const Camera::FrameData& cameraFrameData, const f32 jitterPxX, const f32 jitterPxY, const bool reset)
{
    DLSS::CommonConstantsDesc dlssConstants{};
    dlssConstants.streamlineFrameIndex = m_StreamlineFrameIndex;
    dlssConstants.projectionNoJitter = cameraFrameData.projectionNoJitter;
    dlssConstants.prevProjectionNoJitter = cameraFrameData.prevProjectionNoJitter;
    dlssConstants.invView = cameraFrameData.invView;
    dlssConstants.prevView = cameraFrameData.prevView;
    dlssConstants.cameraPos = cameraFrameData.position;
    dlssConstants.cameraFov = IE_ToRadians(g_Settings.cameraFov);
    dlssConstants.cameraAspect = m_Window.GetAspectRatio();
    dlssConstants.cameraNear = cameraFrameData.znearfar.x;
    dlssConstants.cameraFar = cameraFrameData.znearfar.y;
    dlssConstants.jitterOffsetX = jitterPxX;
    dlssConstants.jitterOffsetY = jitterPxY;
    dlssConstants.reset = reset;
    DLSS::SetCommonConstants(dlssConstants);
}

void Renderer::ReloadRuntimeAndScene(const String& sceneFile)
{
    PrepareRuntimeReload();

    m_BindlessHeaps.ResetAll();
    InvalidateRuntimeDescriptorIndices();

    m_RenderDevice.ClearAllTrackedUploads();

    m_SceneResources.Reset();
    m_Culling.Reset();
    m_TestMovePrev = false;
    m_TestBaseWorlds.clear();

    RecreateRuntimeFrameResources(true);
    LoadScene(sceneFile);
}

void Renderer::CreateSceneDepthSRVs()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        ReleaseSrv(m_BindlessHeaps, m_DepthPre.dsvs[i].srvIndex);
        m_DepthPre.dsvs[i].srvIndex = m_BindlessHeaps.CreateSRV(m_DepthPre.dsvs[i].resource, depthSrvDesc);
    }
}

void Renderer::PresentLoadingFrame()
{
    m_FrameInFlightIdx = m_RenderDevice.GetCurrentBackBufferIndex();

    PerFrameData& frameData = GetCurrentFrameData();
    m_RenderDevice.WaitForFrame(frameData);

    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));

    ComPtr<ID3D12GraphicsCommandList7> cmd = frameData.cmd;
    cmd->RSSetViewports(1, &m_PresentViewport);
    cmd->RSSetScissorRects(1, &m_PresentRect);

    RenderTarget& loadingRt = m_Tonemap.sdrRt[m_FrameInFlightIdx];
    RenderTarget& backBuffer = m_Tonemap.backBufferRt[m_FrameInFlightIdx];
    loadingRt.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &loadingRt.rtv, false, nullptr);

    constexpr FLOAT clearColor[4] = {0.06f, 0.07f, 0.08f, 1.0f};
    cmd->ClearRenderTargetView(loadingRt.rtv, clearColor, 0, nullptr);

    ImGui_FrameStats frameStats{};
    m_Camera.ConfigurePerspective(m_Window.GetAspectRatio(), IE_ToRadians(g_Settings.cameraFov), IE_ToRadians(g_Settings.cameraFrustumCullingFov), 0.1f, 0.0f, 0.0f);
    m_Camera.BuildFrameData();
    const Camera::FrameData cameraFrameData = m_Camera.GetFrameData();
    SubmitDLSSCommonConstants(cameraFrameData, 0.0f, 0.0f, true);

    frameStats.fps = static_cast<u32>(m_Window.GetFPS());
    frameStats.cameraPos[0] = cameraFrameData.position.x;
    frameStats.cameraPos[1] = cameraFrameData.position.y;
    frameStats.cameraPos[2] = cameraFrameData.position.z;
    frameStats.cameraYaw = cameraFrameData.yaw;
    frameStats.cameraPitch = cameraFrameData.pitch;

    ImGui_RenderParams rp{};
    rp.cmd = cmd.Get();
    rp.renderer = this;
    rp.rtv = loadingRt.rtv;
    rp.loadingLabel = m_SceneResources.GetPendingSceneFile().c_str();
    rp.frame = frameStats;
    ImGui_Render(rp);

    DLSS::FrameGenerationTagDesc tagDesc{};
    tagDesc.cmd = cmd.Get();
    tagDesc.streamlineFrameIndex = m_StreamlineFrameIndex;
    tagDesc.valid = false;
    DLSS::TagFrameGenerationInputs(tagDesc);

    DLSS::FrameGenerationOptionsDesc optionsDesc{};
    optionsDesc.enabled = false;
    optionsDesc.renderWidth = m_Upscale.renderSize.x;
    optionsDesc.renderHeight = m_Upscale.renderSize.y;
    optionsDesc.outputWidth = m_Upscale.presentSize.x;
    optionsDesc.outputHeight = m_Upscale.presentSize.y;
    optionsDesc.backBufferFormat = backBuffer.GetDesc().Format;
    optionsDesc.depthFormat = DXGI_FORMAT_D32_FLOAT;
    optionsDesc.motionVectorsFormat = DXGI_FORMAT_R16G16_FLOAT;
    optionsDesc.hudlessFormat = loadingRt.GetDesc().Format;
    optionsDesc.uiFormat = loadingRt.GetDesc().Format;
    DLSS::SetFrameGenerationOptions(optionsDesc);

    loadingRt.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
    backBuffer.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(backBuffer.Get(), loadingRt.Get());
    loadingRt.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    backBuffer.Transition(cmd, D3D12_RESOURCE_STATE_PRESENT);
    m_RenderDevice.ExecuteFrame(frameData, cmd, m_StreamlineFrameIndex);
    CheckFrameGenerationApiError();
    m_StreamlineFrameIndex++;
}

void Renderer::SubmitSceneUploadAndSync(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    m_RenderDevice.ExecuteAndWait(frameData, cmd);
}

void Renderer::ReloadRuntimeForUpscalingConfigChange()
{
    PrepareRuntimeReload();
    RecreateRuntimeFrameResources(false);

    Raytracing& raytracing = m_Raytracing;
    raytracing.CreatePathTracePassResources(m_Upscale.renderSize);

    m_Camera.ResetHistory();
    m_FrameIndex = 0;
}

void Renderer::LoadScene(const String& sceneFile)
{
    const String resolvedScene = m_SceneResources.ResolveSceneName(sceneFile);
    if (!m_Environments.SetCurrentEnvironmentByName(resolvedScene))
    {
        m_Environments.SetCurrentEnvironmentByName("Afternoon");
    }

    Camera& camera = m_Camera;
    camera.LoadSceneConfig(resolvedScene);
    camera.ResetHistory();

    m_FrameInFlightIdx = m_RenderDevice.GetCurrentBackBufferIndex();
    PerFrameData& frameData = GetCurrentFrameData();
    IE_Check(frameData.commandAllocator->Reset());
    IE_Check(frameData.cmd->Reset(frameData.commandAllocator.Get(), nullptr));

    ComPtr<ID3D12GraphicsCommandList7> cmd = frameData.cmd;
    const auto descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(static_cast<UINT>(descriptorHeaps.size()), descriptorHeaps.data());

    const LoadedScene scene = SceneLoader::Load(resolvedScene);
    m_SceneResources.ImportScene(scene, cmd);
    const Vector<Raytracing::RTInstance> rtInstances = m_SceneResources.BuildRTInstances();

    m_Raytracing.Init(cmd, m_Upscale.renderSize, m_SceneResources.GetPrimitives(), scene.primitives, rtInstances);
    SubmitSceneUploadAndSync(frameData, cmd);

    m_SceneResources.SetCurrentSceneFile(resolvedScene);
    m_TestMovePrev = false;
    m_TestBaseWorlds.clear();
    m_FrameIndex = 0;
}

void Renderer::ProcessPendingSceneSwitch()
{
    const bool hasSceneSwitch = m_SceneResources.HasPendingSceneSwitch();
    const bool hasDLSSModeChange = m_HasPendingDLSSModeChange;
    const bool hasFrameGenerationChange = m_HasPendingFrameGenerationChange;
    if (!hasSceneSwitch && !hasDLSSModeChange && !hasFrameGenerationChange)
        return;

    auto applyPendingDLSSModeChange = [&]() {
        if (!m_HasPendingDLSSModeChange)
        {
            return;
        }

        m_DLSSMode = m_PendingDLSSMode;
        m_HasPendingDLSSModeChange = false;
    };

    auto applyPendingFrameGenerationChange = [&]() {
        if (!m_HasPendingFrameGenerationChange)
        {
            return;
        }

        m_FrameGenerationEnabled = m_PendingFrameGenerationEnabled;
        m_HasPendingFrameGenerationChange = false;
    };

    if (!hasSceneSwitch)
    {
        applyPendingDLSSModeChange();
        applyPendingFrameGenerationChange();

        if (m_SceneResources.GetCurrentSceneFile().empty())
        {
            return;
        }

        if (hasDLSSModeChange)
        {
            IE_LogInfo("Reloading runtime resources after DLSS mode change.");
            ReloadRuntimeForUpscalingConfigChange();
        }
        return;
    }

    const String targetScene = m_SceneResources.GetPendingSceneFile();
    if (targetScene.empty())
        return;

    if (m_PendingSceneSwitchDelayFrames > 0)
    {
        --m_PendingSceneSwitchDelayFrames;
        return;
    }

    applyPendingDLSSModeChange();
    applyPendingFrameGenerationChange();
    m_SceneResources.ClearPendingSceneSwitch();
    IE_LogInfo("Switching scene to '{}'", targetScene);
    ReloadRuntimeAndScene(targetScene);
    IE_LogInfo("Scene loaded: '{}'", m_SceneResources.GetCurrentSceneFile());
}

Renderer::PerFrameData& Renderer::GetCurrentFrameData()
{
    return m_RenderDevice.GetCurrentFrameData();
}

u32 Renderer::GetStreamlineFrameIndex() const
{
    return m_StreamlineFrameIndex;
}

Environment& Renderer::GetCurrentEnvironment()
{
    return m_Environments.GetCurrentEnvironment();
}

const Environment& Renderer::GetCurrentEnvironment() const
{
    return m_Environments.GetCurrentEnvironment();
}

const Vector<String>& Renderer::GetEnvironmentNames() const
{
    return m_Environments.GetEnvironmentNames();
}

const String& Renderer::GetCurrentEnvironmentName() const
{
    return m_Environments.GetCurrentEnvironmentName();
}

i32 Renderer::GetCurrentEnvironmentIndex() const
{
    return m_Environments.GetCurrentEnvironmentIndex();
}

void Renderer::SetCurrentEnvironmentIndex(const i32 index)
{
    m_Environments.SetCurrentEnvironmentIndex(index);
}

void Renderer::RequestShaderReload()
{
    m_PendingShaderReload = true;
}

void Renderer::RequestSceneSwitch(const String& sceneFile)
{
    const bool hadPendingSceneSwitch = m_SceneResources.HasPendingSceneSwitch();
    m_SceneResources.RequestSceneSwitch(sceneFile);
    if (!hadPendingSceneSwitch && m_SceneResources.HasPendingSceneSwitch())
    {
        m_PendingSceneSwitchDelayFrames = 1;
    }
}

void Renderer::RequestDLSSMode(DLSS::Mode mode)
{
    if (!m_HasPendingDLSSModeChange && mode == m_DLSSMode)
    {
        return;
    }
    if (m_HasPendingDLSSModeChange && mode == m_PendingDLSSMode)
    {
        return;
    }

    m_PendingDLSSMode = mode;
    m_HasPendingDLSSModeChange = true;
    IE_LogInfo("Queued DLSS mode change.");
}

DLSS::Mode Renderer::GetDLSSMode() const
{
    return m_HasPendingDLSSModeChange ? m_PendingDLSSMode : m_DLSSMode;
}

void Renderer::RequestFrameGenerationEnabled(const bool enabled)
{
    if (!m_HasPendingFrameGenerationChange && enabled == m_FrameGenerationEnabled)
    {
        return;
    }
    if (m_HasPendingFrameGenerationChange && enabled == m_PendingFrameGenerationEnabled)
    {
        return;
    }

    m_PendingFrameGenerationEnabled = enabled;
    m_HasPendingFrameGenerationChange = true;
    IE_LogInfo("Queued DLSS Frame Generation mode change.");
}

bool Renderer::IsFrameGenerationEnabled() const
{
    return m_HasPendingFrameGenerationChange ? m_PendingFrameGenerationEnabled : m_FrameGenerationEnabled;
}

u32 Renderer::GetFrameGenerationPresentedFrames() const
{
    if (!IsFrameGenerationEnabled())
    {
        return 1;
    }

    DLSS::FrameGenerationState fgState{};
    DLSS::GetFrameGenerationState(fgState);
    return IE_Max(fgState.numFramesActuallyPresented, 1u);
}

bool Renderer::HasPendingSceneSwitch() const
{
    return m_SceneResources.HasPendingSceneSwitch();
}

const String& Renderer::GetPendingSceneFile() const
{
    return m_SceneResources.GetPendingSceneFile();
}

const String& Renderer::GetCurrentSceneFile() const
{
    return m_SceneResources.GetCurrentSceneFile();
}

const Vector<SceneUtils::SceneListEntry>& Renderer::GetAvailableScenes() const
{
    return m_SceneResources.GetAvailableScenes();
}

void Renderer::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    m_RenderDevice.SetBufferData(cmd, dst, data, sizeInBytes, offsetInBytes);
}

const ComPtr<ID3D12Device14>& Renderer::GetDevice() const
{
    return m_RenderDevice.GetDevice();
}

Window& Renderer::GetWindow()
{
    return m_Window;
}

const Window& Renderer::GetWindow() const
{
    return m_Window;
}

Camera& Renderer::GetCamera()
{
    return m_Camera;
}

const Camera& Renderer::GetCamera() const
{
    return m_Camera;
}

void Renderer::ApplyTestInstanceMotion()
{
    auto hash32 = [](u32 x) {
        x ^= x >> 16;
        x *= 0x7feb352d;
        x ^= x >> 15;
        x *= 0x846ca68b;
        x ^= x >> 16;
        return x;
    };

    auto hash01 = [](u32 x) { return static_cast<f32>((x >> 8) & 0x00FFFFFF) / static_cast<f32>(0x01000000); };

    if (g_Settings.testMoveInstances)
    {
        Vector<InstanceData>& instances = m_SceneResources.GetInstances();

        if (!m_TestMovePrev || m_TestBaseWorlds.size() != instances.size())
        {
            m_TestBaseWorlds.clear();
            m_TestBaseWorlds.reserve(instances.size());
            for (const InstanceData& inst : instances)
            {
                m_TestBaseWorlds.push_back(inst.world);
            }
        }

        const u32 total = static_cast<u32>(instances.size());
        const u32 want = (g_Settings.testMoveInstancesCount == 0 || g_Settings.testMoveInstancesCount > total) ? total : g_Settings.testMoveInstancesCount;
        const f32 ratio = (total > 0) ? static_cast<f32>(want) / static_cast<f32>(total) : 0.0f;
        const f32 t = static_cast<f32>(m_FrameIndex) * g_Settings.testMoveInstancesSpeed * 0.016f * XM_2PI;
        const f32 amp = g_Settings.testMoveInstancesAmplitude;

        for (u32 i = 0; i < total; ++i)
        {
            const u32 h = hash32(i);
            if (hash01(h) > ratio)
            {
                continue;
            }

            const f32 phase0 = hash01(h ^ 0x9e3779b9u) * XM_2PI;
            const f32 phase1 = hash01(h ^ 0x7f4a7c15u) * XM_2PI;

            const f32 ox = amp * IE_Sinf(t + phase0);
            const f32 oz = amp * IE_Cosf(t + phase1);
            const f32 oy = amp * 0.2f * IE_Sinf(t + phase1);

            XMFLOAT4X4 w = m_TestBaseWorlds[i];
            w._41 += ox;
            w._42 += oy;
            w._43 += oz;
            instances[i].world = w;
        }

        m_SceneResources.MarkInstancesDirty();
        m_TestMovePrev = true;
    }
    else if (m_TestMovePrev)
    {
        Vector<InstanceData>& instances = m_SceneResources.GetInstances();

        if (m_TestBaseWorlds.size() == instances.size())
        {
            for (u32 i = 0; i < instances.size(); ++i)
            {
                instances[i].world = m_TestBaseWorlds[i];
            }
            m_SceneResources.MarkInstancesDirty();
        }
        m_TestBaseWorlds.clear();
        m_TestMovePrev = false;
    }
}
