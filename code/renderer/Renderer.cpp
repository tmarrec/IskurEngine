// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Renderer.h"

#include <directx/d3dx12.h>
#include <dx12/ffx_api_dx12.hpp>

#include "Camera.h"
#include "Constants.h"
#include "ImGui.h"
#include "LoadShader.h"
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
    IE_Check(m_ConstantsBuffer->buffer->Map(0, nullptr, reinterpret_cast<void**>(&m_ConstantsCbMapped)));

    LoadScene();

    m_Environments.Load(m_CommandQueue.Get());

    CreateGPUTimers();

    CreateFSRPassResources();
    CreateGBufferPassResources();
    CreateLightingPassResources();
    CreateHistogramPassResources();

    ReloadShaders();

    ImGui_InitParams imGuiInitParams;
    imGuiInitParams.device = m_Device.Get();
    imGuiInitParams.queue = m_CommandQueue.Get();
    imGuiInitParams.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ImGui_Init(imGuiInitParams);
}

void Renderer::Terminate()
{
    // Wait for last frame execution
    WaitForGpuIdle();

    if (m_ConstantsBuffer && m_ConstantsBuffer->buffer && m_ConstantsCbMapped)
    {
        m_ConstantsBuffer->buffer->Unmap(0, nullptr);
        m_ConstantsCbMapped = nullptr;
    }

    // Raytracing
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
    Pass_GBuffer(cmd);
    Pass_Shadows(cmd);
    Pass_PathTrace(cmd);
    Pass_Lighting(cmd, cameraFrameData);
    Pass_FSR(cmd, jitterNormX, jitterNormY, cameraFrameData);
    Pass_Histogram(cmd);
    Pass_Tonemap(cmd);
    Pass_ImGui(cmd, cameraFrameData);

    EndFrame(frameData, cmd);
}

SharedPtr<Buffer> Renderer::CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc)
{
    IE_Assert(createDesc.sizeInBytes > 0);

    const bool hasInitData = (createDesc.initialData != nullptr) && (createDesc.initialDataSize > 0);

    if (hasInitData)
    {
        IE_Assert(createDesc.initialDataSize <= createDesc.sizeInBytes);
    }
    else
    {
        IE_Assert(createDesc.initialData == nullptr || createDesc.initialDataSize == 0);
    }

    // Views imply you know how the buffer is viewed.
    if (createDesc.createSRV || createDesc.createUAV)
    {
        IE_Assert(createDesc.viewKind != BufferCreateDesc::ViewKind::None);
    }

    // UAV view only makes sense on DEFAULT heap.
    if (createDesc.createUAV)
    {
        IE_Assert(createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT);
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
    }

    // Heap-specific sanity
    if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0);
        IE_Assert(!createDesc.createUAV);
        IE_Assert(createDesc.initialState == D3D12_RESOURCE_STATE_COMMON || createDesc.initialState == D3D12_RESOURCE_STATE_GENERIC_READ);
        IE_Assert(createDesc.finalState == D3D12_RESOURCE_STATE_COMMON || createDesc.finalState == D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    else if (createDesc.heapType == D3D12_HEAP_TYPE_READBACK)
    {
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0);
        IE_Assert(!createDesc.createUAV);
        IE_Assert(!hasInitData); // init-from-CPU into a readback resource is almost certainly a bug
        IE_Assert(createDesc.initialState == D3D12_RESOURCE_STATE_COMMON || createDesc.initialState == D3D12_RESOURCE_STATE_COPY_DEST);
        IE_Assert(createDesc.finalState == D3D12_RESOURCE_STATE_COMMON || createDesc.finalState == D3D12_RESOURCE_STATE_COPY_DEST);
    }

    SharedPtr<Buffer> out = IE_MakeSharedPtr<Buffer>();

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(createDesc.sizeInBytes, createDesc.resourceFlags);

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = createDesc.heapType;

    // Track the *real* state at creation.
    D3D12_RESOURCE_STATES createState = D3D12_RESOURCE_STATE_COMMON;

    if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        createState = D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    else if (createDesc.heapType == D3D12_HEAP_TYPE_READBACK)
    {
        createState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    else if (createDesc.initialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        createState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }

    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, createState, nullptr, out->allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS(out->buffer.ReleaseAndGetAddressOf())));

    IE_Check(out->buffer->SetName(createDesc.name));
    out->state = createState;

    // Decide the state the caller wants the returned buffer to be in.
    // If init data exists and finalState is provided (non-COMMON), use it; otherwise fall back to initialState.
    D3D12_RESOURCE_STATES desiredState = createDesc.initialState;
    if (hasInitData && createDesc.finalState != D3D12_RESOURCE_STATE_COMMON)
    {
        desiredState = createDesc.finalState;
    }

    auto TransitionIfNeeded = [&](D3D12_RESOURCE_STATES newState) {
        if (!cmd || out->state == newState)
            return;

        const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(out->buffer.Get(), out->state, newState);
        cmd->ResourceBarrier(1, &b);
        out->state = newState;
    };

    // Init data
    if (hasInitData)
    {
        if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            u8* mapped = nullptr;

            const CD3DX12_RANGE readRange(0, 0); // CPU won't read
            IE_Check(out->buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped, createDesc.initialData, createDesc.initialDataSize);

            const CD3DX12_RANGE writeRange(0, createDesc.initialDataSize);
            out->buffer->Unmap(0, &writeRange);
        }
        else
        {
            IE_Assert(createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT);
            IE_Assert(cmd != nullptr);

            UploadTemp staging{};
            const D3D12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(createDesc.initialDataSize);

            D3D12MA::ALLOCATION_DESC upAlloc{};
            upAlloc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

            IE_Check(m_Allocator->CreateResource(&upAlloc, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, staging.allocation.ReleaseAndGetAddressOf(),
                                                 IID_PPV_ARGS(staging.resource.ReleaseAndGetAddressOf())));

            IE_Check(staging.resource->SetName(L"BufferInit/Upload"));

            u8* mapped = nullptr;
            const CD3DX12_RANGE readRange(0, 0);
            IE_Check(staging.resource->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped, createDesc.initialData, createDesc.initialDataSize);
            const CD3DX12_RANGE writeRange(0, createDesc.initialDataSize);
            staging.resource->Unmap(0, &writeRange);

            TransitionIfNeeded(D3D12_RESOURCE_STATE_COPY_DEST);

            cmd->CopyBufferRegion(out->buffer.Get(), 0, staging.resource.Get(), 0, createDesc.initialDataSize);

            if (desiredState != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                TransitionIfNeeded(desiredState);
            }

            m_InFlightUploads[m_FrameInFlightIdx].push_back(std::move(staging));
        }
    }
    else
    {
        // No init data: if caller wants a non-COMMON state and we have a cmd, transition now.
        if (cmd && createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT && createDesc.initialState != D3D12_RESOURCE_STATE_COMMON)
        {
            TransitionIfNeeded(createDesc.initialState);
        }
    }

    // Views
    out->srvIndex = UINT32_MAX;
    out->uavIndex = UINT32_MAX;
    out->numElements = 0;

    if (createDesc.viewKind == BufferCreateDesc::ViewKind::Structured)
    {
        IE_Assert(createDesc.strideInBytes > 0);
        IE_Assert((createDesc.strideInBytes & 3u) == 0);
        IE_Assert(createDesc.strideInBytes <= 2048u);
        IE_Assert((createDesc.sizeInBytes % createDesc.strideInBytes) == 0);

        const u64 n64 = createDesc.sizeInBytes / createDesc.strideInBytes;
        IE_Assert(n64 <= UINT32_MAX);
        const u32 n = static_cast<u32>(n64);

        out->numElements = n;

        if (createDesc.createSRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = n;
            srv.Buffer.StructureByteStride = createDesc.strideInBytes;
            out->srvIndex = m_BindlessHeaps.CreateSRV(out->buffer, srv);
        }

        if (createDesc.createUAV)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = n;
            uav.Buffer.StructureByteStride = createDesc.strideInBytes;
            out->uavIndex = m_BindlessHeaps.CreateUAV(out->buffer, uav);
        }
    }
    else if (createDesc.viewKind == BufferCreateDesc::ViewKind::Raw)
    {
        IE_Assert((createDesc.sizeInBytes & 3u) == 0);

        const u64 n64 = createDesc.sizeInBytes / 4u;
        IE_Assert(n64 <= UINT32_MAX);
        const u32 n = static_cast<u32>(n64);

        out->numElements = n;

        if (createDesc.createSRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = n;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            out->srvIndex = m_BindlessHeaps.CreateSRV(out->buffer, srv);
        }

        if (createDesc.createUAV)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = n;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            out->uavIndex = m_BindlessHeaps.CreateUAV(out->buffer, uav);
        }
    }
    else
    {
        IE_Assert(!createDesc.createSRV);
        IE_Assert(!createDesc.createUAV);
    }

    return out;
}

void Renderer::BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY)
{
    WaitOnFence(frameData.frameFence, frameData.frameFenceValue);

    m_InFlightUploads[m_FrameInFlightIdx].clear();

    if (m_PendingShaderReload)
    {
        g_ShadersCompilationSuccess = true;
        ReloadShaders();
        m_PendingShaderReload = false;
        IE_Log("{}", g_ShadersCompilationSuccess);
    }

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

    Environment& env = m_Environments.GetCurrentEnvironment();

    // Sun
    float cosE = cosf(g_Sun_Elevation);
    float sinE = sinf(g_Sun_Elevation);
    XMVECTOR sun = XMVectorSet(cosE * cosf(g_Sun_Azimuth), sinE, cosE * sinf(g_Sun_Azimuth), 0.0f);
    sun = XMVector3Normalize(sun);
    XMStoreFloat3(&env.sunDir, sun);

    cameraFrameData = camera.GetFrameData();

    // Constant Buffer
    VertexConstants constants;
    constants.cameraPos = cameraFrameData.position;
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
    // Transition
    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

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
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->buffer->GetGPUVirtualAddress() + static_cast<u64>(m_FrameInFlightIdx) * m_ConstantsCbStride);
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
        cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->buffer->GetGPUVirtualAddress() + static_cast<u64>(m_FrameInFlightIdx) * m_ConstantsCbStride);
        for (CullMode cm : {CullMode_Back, CullMode_None})
        {
            cmd->SetPipelineState(m_DepthPre.alphaTestPSO[cm].Get());
            drawPrimitives(m_PrimitivesRenderData[AlphaMode_Mask][cm]);
        }
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
}

void Renderer::Pass_Shadows(const ComPtr<ID3D12GraphicsCommandList7>& cmd) const
{
    Raytracing& raytracing = Raytracing::GetInstance();
    const Environment& env = m_Environments.GetCurrentEnvironment();

    if (g_RTShadows_Enabled)
    {
        Raytracing::ShadowPassInput shadowPassInput;
        shadowPassInput.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
        shadowPassInput.sunDir = env.sunDir;
        shadowPassInput.frameIndex = m_FrameIndex;
        raytracing.ShadowPass(cmd, shadowPassInput);
    }
}

void Renderer::Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    PerFrameData& frameData = GetCurrentFrameData();

    auto& gb = m_GBuf.gbuffers[m_FrameInFlightIdx];

    auto BarrierGBuffer = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        Barrier(cmd, gb.albedo, from, to);
        Barrier(cmd, gb.normal, from, to);
        Barrier(cmd, gb.normalGeo, from, to);
        Barrier(cmd, gb.material, from, to);
        Barrier(cmd, gb.motionVector, from, to);
        Barrier(cmd, gb.ao, from, to);
    };

    BarrierGBuffer(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

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
        const char* passName = alphaMode == AlphaMode_Opaque ? "GBuffer - Opaque" : "GBuffer - Masked";
        GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, passName);
        {
            cmd->SetDescriptorHeaps(m_BindlessHeaps.GetDescriptorHeaps().size(), m_BindlessHeaps.GetDescriptorHeaps().data());
            cmd->SetGraphicsRootSignature(m_GBuf.rootSigs[alphaMode].Get());
            cmd->OMSetRenderTargets(GBuffer::targetCount, m_GBuf.rtvHandles[m_FrameInFlightIdx].data(), false, &m_DepthPre.dsvHandles[m_FrameInFlightIdx]);
            cmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->buffer->GetGPUVirtualAddress() + static_cast<u64>(m_FrameInFlightIdx) * m_ConstantsCbStride);

            cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_Back].Get());
            drawPrimitives(m_PrimitivesRenderData[alphaMode][CullMode_Back]);

            cmd->SetPipelineState(m_GBuf.psos[alphaMode][CullMode_None].Get());
            drawPrimitives(m_PrimitivesRenderData[alphaMode][CullMode_None]);
        }
        GPU_MARKER_END(cmd, frameData.gpuTimers);
    }

    BarrierGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    Barrier(cmd, m_DepthPre.dsvs[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::Pass_PathTrace(const ComPtr<ID3D12GraphicsCommandList7>& cmd) const
{
    Raytracing& raytracing = Raytracing::GetInstance();
    const Environment& env = m_Environments.GetCurrentEnvironment();

    Raytracing::PathTracePassInput pathTracePassInput;
    pathTracePassInput.depthTextureIndex = m_DepthPre.dsvSrvIdx[m_FrameInFlightIdx];
    pathTracePassInput.sunDir = env.sunDir;
    pathTracePassInput.frameIndex = m_FrameIndex;
    pathTracePassInput.normalGeoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].normalGeoIndex;
    pathTracePassInput.albedoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].albedoIndex;
    pathTracePassInput.materialsBufferIndex = m_MaterialsBuffer->srvIndex;
    pathTracePassInput.samplerIndex = m_LinearSamplerIdx;
    raytracing.PathTracePass(cmd, pathTracePassInput);
}

void Renderer::Pass_Lighting(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData)
{
    PerFrameData& frameData = GetCurrentFrameData();
    const Raytracing::ShadowPassResources& shadowPassResources = Raytracing::GetInstance().GetShadowPassResources();
    const Raytracing::PathTracePassResources& pathTracePassResources = Raytracing::GetInstance().GetPathTracePassResources();
    const Environment& env = m_Environments.GetCurrentEnvironment();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Lighting");
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
        c.sunDir = env.sunDir;
        c.raytracingOutputIndex = shadowPassResources.trace.outputSrvIndex;
        c.envMapIndex = env.envSrvIdx;
        c.diffuseIBLIndex = env.diffuseSrvIdx;
        c.specularIBLIndex = env.specularSrvIdx;
        c.brdfLUTIndex = env.brdfSrvIdx;
        c.sunAzimuth = g_Sun_Azimuth;
        c.IBLSpecularIntensity = g_IBL_SpecularIntensity;
        c.RTShadowsEnabled = g_RTShadows_Enabled;
        c.renderSize = {static_cast<f32>(m_Fsr.renderSize.x), static_cast<f32>(m_Fsr.renderSize.y)};
        c.sunIntensity = g_Sun_Intensity;
        c.skyIntensity = g_IBL_SkyIntensity;
        c.aoTextureIndex = m_GBuf.gbuffers[m_FrameInFlightIdx].aoIndex;
        c.indirectDiffuseTextureIndex = pathTracePassResources.trace.outputSrvIndex;
        std::memcpy(m_Light.cbMapped[m_FrameInFlightIdx], &c, sizeof(c));

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

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Histogram");
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

        const Raytracing::ShadowPassResources& shadowPassResources = Raytracing::GetInstance().GetShadowPassResources();
        const Raytracing::PathTracePassResources& pathTracePassResources = Raytracing::GetInstance().GetPathTracePassResources();

        ImGui_RenderParams rp;
        rp.cmd = cmd.Get();
        rp.rtv = m_Tonemap.rtvHandles[m_FrameInFlightIdx];
        rp.rtvResource = m_Tonemap.sdrRt[m_FrameInFlightIdx].Get();
        rp.gbufferAlbedo = m_GBuf.gbuffers[m_FrameInFlightIdx].albedo.Get();
        rp.gbufferNormal = m_GBuf.gbuffers[m_FrameInFlightIdx].normal.Get();
        rp.gbufferNormalGeo = m_GBuf.gbuffers[m_FrameInFlightIdx].normalGeo.Get();
        rp.gbufferMaterial = m_GBuf.gbuffers[m_FrameInFlightIdx].material.Get();
        rp.gbufferMotion = m_GBuf.gbuffers[m_FrameInFlightIdx].motionVector.Get();
        rp.gbufferAO = m_GBuf.gbuffers[m_FrameInFlightIdx].ao.Get();
        rp.depth = m_DepthPre.dsvs[m_FrameInFlightIdx].Get();
        rp.rtShadows = shadowPassResources.trace.outputTexture.Get();
        rp.rtIndirectDiffuse = pathTracePassResources.trace.indirectDiffuseTexture.Get();
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

void Renderer::EndFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    if (frameData.gpuTimers.nextIdx)
    {
        cmd->ResolveQueryData(frameData.gpuTimers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, frameData.gpuTimers.nextIdx, frameData.gpuTimers.readback.Get(), 0);
    }

    Barrier(cmd, m_Tonemap.sdrRt[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    IE_Check(cmd->Close());
    ID3D12CommandList* pCmdList = cmd.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmdList);
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), frameData.frameFenceValue));

    IE_Check(m_Swapchain->Present(0, 0));
    m_FrameIndex++;
}

void Renderer::WaitForGpuIdle()
{
    ComPtr<ID3D12Fence> fence;
    IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    const UINT64 value = 1;
    IE_Check(m_CommandQueue->Signal(fence.Get(), value));

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    IE_Check(fence->SetEventOnCompletion(value, evt));
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
}

void Renderer::ReloadShaders()
{
    WaitForGpuIdle();

    IE_Log("Reloading shaders...");

    Vector<WString> globalDefines;
    CreateDepthPrePassPipelines(globalDefines);
    CreateGBufferPassPipelines(globalDefines);
    CreateLightingPassPipelines(globalDefines);
    CreateHistogramPassPipelines(globalDefines);
    CreateToneMapPassPipelines(globalDefines);

    Raytracing::GetInstance().ReloadShaders();

    IE_Log("Shader reload done.");
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

        IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                                             m_DepthPre.dsvAllocs[i].ReleaseAndGetAddressOf(), IID_PPV_ARGS(&m_DepthPre.dsvs[i])));
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

    ffx::CreateContextDescUpscaleVersion upscalerVersion{};
    upscalerVersion.version = FFX_UPSCALER_VERSION;

    ffx::CreateContextDescUpscale createUpscaling{};
    createUpscaling.maxRenderSize = {m_Fsr.renderSize.x, m_Fsr.renderSize.y};
    createUpscaling.maxUpscaleSize = {m_Fsr.presentSize.x, m_Fsr.presentSize.y};
    createUpscaling.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    IE_Assert(ffx::CreateContext(m_Fsr.context, nullptr, createUpscaling, backendDesc, upscalerVersion) == ffx::ReturnCode::Ok);

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

        m_Fsr.outputState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}

void Renderer::CreateDepthPrePassPipelines(const Vector<WString>& globalDefines)
{
    m_GBuf.amplificationShader = IE_LoadShader(IE_SHADER_TYPE_AMPLIFICATION, L"amplification/asBasic.hlsl", {globalDefines}, m_GBuf.amplificationShader);
    m_GBuf.meshShader = IE_LoadShader(IE_SHADER_TYPE_MESH, L"mesh/msBasic.hlsl", {globalDefines}, m_GBuf.meshShader);

    CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
    ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    // Opaque
    {
        IE_Check(m_Device->CreateRootSignature(0, m_GBuf.meshShader->blob->GetBufferPointer(), m_GBuf.meshShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_DepthPre.opaqueRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.opaqueRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader->blob->GetBufferSize() > 0
                           ? D3D12_SHADER_BYTECODE(m_GBuf.amplificationShader->blob->GetBufferPointer(), m_GBuf.amplificationShader->blob->GetBufferSize())
                           : D3D12_SHADER_BYTECODE();
        depthDesc.MS = D3D12_SHADER_BYTECODE(m_GBuf.meshShader->blob->GetBufferPointer(), m_GBuf.meshShader->blob->GetBufferSize());
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
        m_DepthPre.alphaTestShader = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psAlphaTest.hlsl", {globalDefines}, m_DepthPre.alphaTestShader);

        IE_Check(m_Device->CreateRootSignature(0, m_DepthPre.alphaTestShader->blob->GetBufferPointer(), m_DepthPre.alphaTestShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_DepthPre.alphaTestRootSig)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC depthDesc{};
        depthDesc.pRootSignature = m_DepthPre.alphaTestRootSig.Get();
        depthDesc.AS = m_GBuf.amplificationShader->blob->GetBufferSize() > 0
                           ? D3D12_SHADER_BYTECODE(m_GBuf.amplificationShader->blob->GetBufferPointer(), m_GBuf.amplificationShader->blob->GetBufferSize())
                           : D3D12_SHADER_BYTECODE();
        depthDesc.MS = D3D12_SHADER_BYTECODE(m_GBuf.meshShader->blob->GetBufferPointer(), m_GBuf.meshShader->blob->GetBufferSize());
        depthDesc.PS = D3D12_SHADER_BYTECODE(m_DepthPre.alphaTestShader->blob->GetBufferPointer(), m_DepthPre.alphaTestShader->blob->GetBufferSize());
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

constexpr DXGI_FORMAT formats[GBuffer::targetCount] = {
    DXGI_FORMAT_R8G8B8A8_UNORM, // Albedo
    DXGI_FORMAT_R16G16_FLOAT,   // Normal
    DXGI_FORMAT_R16G16_FLOAT,   // NormalGeo
    DXGI_FORMAT_R16G16_UNORM,   // Material
    DXGI_FORMAT_R16G16_FLOAT,   // Motion vector
    DXGI_FORMAT_R8_UNORM,       // AO
};
constexpr const wchar_t* rtvNames[GBuffer::targetCount] = {L"GBuffer Albedo", L"GBuffer Normal", L"GBuffer Normal Geometry", L"GBuffer Material", L"GBuffer Motion Vector", L"GBuffer AO"};

void Renderer::CreateGBufferPassPipelines(const Vector<WString>& globalDefines)
{
    m_GBuf.pixelShaders[AlphaMode_Opaque] = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psGBuffer.hlsl", globalDefines, m_GBuf.pixelShaders[AlphaMode_Opaque]);

    Vector<WString> blendDefines = globalDefines;
    blendDefines.push_back(L"ENABLE_BLEND");
    m_GBuf.pixelShaders[AlphaMode_Blend] = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psGBuffer.hlsl", blendDefines, m_GBuf.pixelShaders[AlphaMode_Blend]);

    Vector<WString> maskDefines = globalDefines;
    maskDefines.push_back(L"ENABLE_ALPHA_TEST");
    m_GBuf.pixelShaders[AlphaMode_Mask] = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psGBuffer.hlsl", maskDefines, m_GBuf.pixelShaders[AlphaMode_Mask]);

    for (AlphaMode alphaMode = AlphaMode_Opaque; alphaMode < AlphaMode_Count; alphaMode = static_cast<AlphaMode>(alphaMode + 1))
    {
        IE_Check(m_Device->CreateRootSignature(0, m_GBuf.meshShader->blob->GetBufferPointer(), m_GBuf.meshShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_GBuf.rootSigs[alphaMode])));

        CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
        ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC msDesc{};
        msDesc.pRootSignature = m_GBuf.rootSigs[alphaMode].Get();
        msDesc.AS = m_GBuf.amplificationShader->blob->GetBufferSize() > 0
                        ? D3D12_SHADER_BYTECODE(m_GBuf.amplificationShader->blob->GetBufferPointer(), m_GBuf.amplificationShader->blob->GetBufferSize())
                        : D3D12_SHADER_BYTECODE();
        msDesc.MS = D3D12_SHADER_BYTECODE(m_GBuf.meshShader->blob->GetBufferPointer(), m_GBuf.meshShader->blob->GetBufferSize());
        msDesc.PS = D3D12_SHADER_BYTECODE(m_GBuf.pixelShaders[alphaMode]->blob->GetBufferPointer(), m_GBuf.pixelShaders[alphaMode]->blob->GetBufferSize());
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

void Renderer::CreateLightingPassPipelines(const Vector<WString>& globalDefines)
{
    m_Light.pxShader = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psLighting.hlsl", {globalDefines}, m_Light.pxShader);
    m_Light.vxShader = IE_LoadShader(IE_SHADER_TYPE_VERTEX, L"vertex/vsFullscreen.hlsl", {globalDefines}, m_Light.vxShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Light.pxShader->blob->GetBufferPointer(), m_Light.pxShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Light.rootSig)));

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_Light.rootSig.Get();
    psoDesc.VS.pShaderBytecode = m_Light.vxShader->blob->GetBufferPointer();
    psoDesc.VS.BytecodeLength = m_Light.vxShader->blob->GetBufferSize();
    psoDesc.PS.pShaderBytecode = m_Light.pxShader->blob->GetBufferPointer();
    psoDesc.PS.BytecodeLength = m_Light.pxShader->blob->GetBufferSize();
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    psoDesc.SampleDesc = DefaultSampleDesc();
    IE_Check(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_Light.pso)));
}

void Renderer::CreateHistogramPassPipelines(const Vector<WString>& globalDefines)
{
    // Clear pass
    m_Histo.clearUintShader = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/csClearUint.hlsl", {globalDefines}, m_Histo.clearUintShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.clearUintShader->blob->GetBufferPointer(), m_Histo.clearUintShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Histo.clearUintRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC clearUintPsoDesc{};
    clearUintPsoDesc.pRootSignature = m_Histo.clearUintRootSig.Get();
    clearUintPsoDesc.CS.pShaderBytecode = m_Histo.clearUintShader->blob->GetBufferPointer();
    clearUintPsoDesc.CS.BytecodeLength = m_Histo.clearUintShader->blob->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&clearUintPsoDesc, IID_PPV_ARGS(&m_Histo.clearUintPso)));

    // Histogram pass
    m_Histo.histogramShader = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/csHistogram.hlsl", {globalDefines}, m_Histo.histogramShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.histogramShader->blob->GetBufferPointer(), m_Histo.histogramShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Histo.histogramRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC histogramPsoDesc{};
    histogramPsoDesc.pRootSignature = m_Histo.histogramRootSig.Get();
    histogramPsoDesc.CS.pShaderBytecode = m_Histo.histogramShader->blob->GetBufferPointer();
    histogramPsoDesc.CS.BytecodeLength = m_Histo.histogramShader->blob->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&histogramPsoDesc, IID_PPV_ARGS(&m_Histo.histogramPso)));

    // Exposure pass
    m_Histo.exposureShader = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/csExposure.hlsl", {globalDefines}, m_Histo.exposureShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.exposureShader->blob->GetBufferPointer(), m_Histo.exposureShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Histo.exposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC exposurePsoDesc{};
    exposurePsoDesc.pRootSignature = m_Histo.exposureRootSig.Get();
    exposurePsoDesc.CS.pShaderBytecode = m_Histo.exposureShader->blob->GetBufferPointer();
    exposurePsoDesc.CS.BytecodeLength = m_Histo.exposureShader->blob->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&exposurePsoDesc, IID_PPV_ARGS(&m_Histo.exposurePso)));

    // Adapt exposure pass
    m_Histo.adaptExposureShader = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/csAdaptExposure.hlsl", {globalDefines}, m_Histo.adaptExposureShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Histo.adaptExposureShader->blob->GetBufferPointer(), m_Histo.adaptExposureShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Histo.adaptExposureRootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC adaptExposurePsoDesc{};
    adaptExposurePsoDesc.pRootSignature = m_Histo.adaptExposureRootSig.Get();
    adaptExposurePsoDesc.CS.pShaderBytecode = m_Histo.adaptExposureShader->blob->GetBufferPointer();
    adaptExposurePsoDesc.CS.BytecodeLength = m_Histo.adaptExposureShader->blob->GetBufferSize();
    IE_Check(m_Device->CreateComputePipelineState(&adaptExposurePsoDesc, IID_PPV_ARGS(&m_Histo.adaptExposurePso)));
}

void Renderer::CreateToneMapPassPipelines(const Vector<WString>& globalDefines)
{
    m_Tonemap.vxShader = IE_LoadShader(IE_SHADER_TYPE_VERTEX, L"vertex/vsFullscreen.hlsl", {globalDefines}, m_Tonemap.vxShader);
    m_Tonemap.pxShader = IE_LoadShader(IE_SHADER_TYPE_PIXEL, L"pixel/psTonemap.hlsl", {globalDefines}, m_Tonemap.pxShader);
    IE_Check(m_Device->CreateRootSignature(0, m_Tonemap.pxShader->blob->GetBufferPointer(), m_Tonemap.pxShader->blob->GetBufferSize(), IID_PPV_ARGS(&m_Tonemap.rootSig)));

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_Tonemap.rootSig.Get();
    psoDesc.VS.pShaderBytecode = m_Tonemap.vxShader->blob->GetBufferPointer();
    psoDesc.VS.BytecodeLength = m_Tonemap.vxShader->blob->GetBufferSize();
    psoDesc.PS.pShaderBytecode = m_Tonemap.pxShader->blob->GetBufferPointer();
    psoDesc.PS.BytecodeLength = m_Tonemap.pxShader->blob->GetBufferSize();
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

void Renderer::CreateGBufferPassResources()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        GBuffer& gbuff = m_GBuf.gbuffers[i];
        ComPtr<ID3D12Resource>* targets[GBuffer::targetCount] = {&gbuff.albedo, &gbuff.normal, &gbuff.normalGeo, &gbuff.material, &gbuff.motionVector, &gbuff.ao};

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

            IE_Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(targets[t]->GetAddressOf())));
            IE_Check(targets[t]->Get()->SetName(rtvNames[t]));

            m_Device->CreateRenderTargetView(targets[t]->Get(), nullptr, rtvHandle);
            m_GBuf.rtvHandles[i][t] = rtvHandle;

            rtvHandle.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
}

void Renderer::CreateLightingPassResources()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv2D{};
    srv2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv2D.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv2D.Texture2D.MipLevels = 1;

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(IE_AlignUp(sizeof(LightingPassConstants), 256));
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        srv2D.Format = m_GBuf.gbuffers[i].albedo->GetDesc().Format;
        m_GBuf.gbuffers[i].albedoIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].albedo, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].normal->GetDesc().Format;
        m_GBuf.gbuffers[i].normalIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].normal, srv2D);

        srv2D.Format = m_GBuf.gbuffers[i].normalGeo->GetDesc().Format;
        m_GBuf.gbuffers[i].normalGeoIndex = m_BindlessHeaps.CreateSRV(m_GBuf.gbuffers[i].normalGeo, srv2D);

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

void Renderer::CreateHistogramPassResources()
{
    BufferCreateDesc bufferDesc{};
    bufferDesc.heapType = D3D12_HEAP_TYPE_DEFAULT;
    bufferDesc.viewKind = BufferCreateDesc::ViewKind::Structured;
    bufferDesc.createSRV = true;
    bufferDesc.createUAV = true;
    bufferDesc.resourceFlags = D3D12_RESOURCE_FLAG_NONE | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bufferDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bufferDesc.finalState = bufferDesc.initialState;

    bufferDesc.sizeInBytes = m_Histo.numBuckets * sizeof(u32);
    bufferDesc.strideInBytes = sizeof(u32);
    bufferDesc.name = L"Histogram";
    m_Histo.histogramBuffer = CreateBuffer(nullptr, bufferDesc);

    bufferDesc.sizeInBytes = 1 * sizeof(f32);
    bufferDesc.strideInBytes = sizeof(f32);
    bufferDesc.name = L"Exposure";
    m_Histo.exposureBuffer = CreateBuffer(nullptr, bufferDesc);

    bufferDesc.sizeInBytes = 1 * sizeof(f32);
    bufferDesc.strideInBytes = sizeof(f32);
    bufferDesc.name = L"Adapt Exposure";
    m_Histo.adaptExposureBuffer = CreateBuffer(nullptr, bufferDesc);
}

void Renderer::LoadScene()
{
    const CommandLineArguments& args = GetCommandLineArguments();
    String sceneFile = args.sceneFile.empty() ? "Bistro" : args.sceneFile;

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

const Environment& Renderer::GetCurrentEnvironment() const
{
    return m_Environments.GetCurrentEnvironment();
}

void Renderer::RequestShaderReload()
{
    m_PendingShaderReload = true;
}

void Renderer::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    UploadTemp staging{};

    D3D12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    D3D12MA::ALLOCATION_DESC upAlloc{};
    upAlloc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    IE_Check(m_Allocator->CreateResource(&upAlloc, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, staging.allocation.ReleaseAndGetAddressOf(),
                                         IID_PPV_ARGS(staging.resource.ReleaseAndGetAddressOf())));
    IE_Check(staging.resource->SetName(L"SetBufferData/Upload"));

    u8* mapped = nullptr;
    IE_Check(staging.resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memcpy(mapped, data, sizeInBytes);
    staging.resource->Unmap(0, nullptr);

    const D3D12_RESOURCE_STATES before = dst->state;
    Barrier(cmd, dst->buffer, before, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(dst->buffer.Get(), offsetInBytes, staging.resource.Get(), 0, sizeInBytes);
    Barrier(cmd, dst->buffer, D3D12_RESOURCE_STATE_COPY_DEST, before);

    m_InFlightUploads[m_FrameInFlightIdx].push_back(staging);
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
