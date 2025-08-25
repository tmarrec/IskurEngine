// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <pix.h>

#include "AlphaMode.h"
#include "BindlessHeaps.h"
#include "Buffer.h"
#include "CompileShader.h"
#include "Constants.h"
#include "Shader.h"
#include "World.h"

#include "../common/Singleton.h"
#include "ffx_api/ffx_api.hpp"

enum class RayTracingResolution : u32
{
    Full = 0,        // Full resolution (x, y)
    FullX_HalfY = 1, // Full resolution in x, half resolution in y
    Half = 2,        // Both x and y at half resolution
    Quarter = 3      // Both x and y at quarter resolution
};

struct GBuffer
{
    static constexpr u32 targetCount = 4;

    ComPtr<ID3D12Resource> albedo;
    ComPtr<ID3D12Resource> normal;
    ComPtr<ID3D12Resource> material;
    ComPtr<ID3D12Resource> motionVector;

    u32 albedoIndex = UINT32_MAX;
    u32 normalIndex = UINT32_MAX;
    u32 materialIndex = UINT32_MAX;
    u32 motionVectorIndex = UINT32_MAX;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
};

class Renderer : public Singleton<Renderer>
{
  public:
    void Init();
    void Terminate();

    void Render();

    SharedPtr<Buffer> CreateStructuredBuffer(u32 sizeInBytes, u32 strideInBytes, const WString& name, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT);
    SharedPtr<Buffer> CreateBytesBuffer(u32 numElements, const WString& name, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT);

    static Shader LoadShader(ShaderType type, const WString& filename, const Vector<WString>& defines);

    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);
    static void SetResourceBufferData(const ComPtr<ID3D12Resource>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    static void Barrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);
    static void UAVBarrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource);

    void AllocateUploadBuffer(const void* pData, u32 sizeInBytes, u32 offsetInBytes, ComPtr<ID3D12Resource>& resource, ComPtr<D3D12MA::Allocation>& allocation, const wchar_t* resourceName) const;
    void AllocateUAVBuffer(u32 sizeInBytes, ComPtr<ID3D12Resource>& resource, ComPtr<D3D12MA::Allocation>& allocation, D3D12_RESOURCE_STATES initialResourceState, const wchar_t* resourceName) const;

    struct GpuTimers
    {
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> readback;
        u32 nextIdx;
        struct Pass
        {
            const char* name;
            u32 idxBegin;
            u32 idxEnd;
            f64 ms;
        } passes[128];
        u32 passCount;
    };
    struct PerFrameData
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList7> cmd;

        ComPtr<ID3D12Fence> frameFence;
        u64 frameFenceValue;

        GpuTimers gpuTimers;
    };

  private:
    void CreateDevice();
    void CreateAllocator();
    void CreateCommandQueue();
    void CreateSwapchain();
    void CreateFrameSynchronizationFences();
    void CreateCommands();
    void CreateRTVs();
    void CreateDSV();

    void SetRenderAndPresentSize();

    void CreateGPUTimers();
    void CollectGpuTimings(PerFrameData& frameData);

    void CreateFSRPassResources();
    void CreateDepthPrePassResources(const Vector<WString>& globalDefines);
    void CreateGBufferPassResources(const Vector<WString>& globalDefines);
    void CreateLightingPassResources(const Vector<WString>& globalDefines);
    void CreateHistogramPassResources(const Vector<WString>& globalDefines);
    void CreateToneMapPassResources(const Vector<WString>& globalDefines);
    void CreateSSAOResources(const Vector<WString>& globalDefines);

    void SetupImGui();

    void LoadScene();

    static void WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue);

    void SetupRaytracing(ComPtr<ID3D12GraphicsCommandList7>& cmdList);

    ComPtr<ID3D12Device14> m_Device;
    ComPtr<ID3D12Debug> m_Debug;
    ComPtr<IDXGIFactory6> m_DxgiFactory;
    ComPtr<D3D12MA::Allocator> m_Allocator;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain4> m_Swapchain;

    BindlessHeaps m_BindlessHeaps;

    Array<PerFrameData, IE_Constants::frameInFlightCount> m_AllFrameData{};
    PerFrameData& GetCurrentFrameData();

    u32 m_FrameInFlightIdx = 0;
    u32 m_FrameIndex = 0;

    ComPtr<D3D12MA::Allocation> m_ConstantsBufferAlloc;
    ComPtr<ID3D12Resource> m_ConstantsBuffer;

    D3D12_VIEWPORT m_RenderViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_RenderRect = {0, 0, 0, 0};
    D3D12_VIEWPORT m_PresentViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_PresentRect = {0, 0, 0, 0};

    float3 m_SunDir = {0.3f, 1.f, 0.75f};

    Vector<Material> m_Materials;
    SharedPtr<Buffer> m_MaterialsBuffer;

    SharedPtr<World> m_World;

    Array<Array<Vector<SharedPtr<Primitive>>, 2>, AlphaMode_Count> m_PrimitivesPerAlphaMode; // not really safe but good enough for now
    Vector<SharedPtr<Primitive>> m_Primitives;                                               // not really safe but good enough for now

    u32 m_LinearSamplerIdx = UINT_MAX;

    // Raytracing pass
    ComPtr<ID3D12RootSignature> m_RaytracingGlobalRootSignature;
    ComPtr<ID3D12StateObject> m_DxrStateObject;

    ComPtr<D3D12MA::Allocation> m_TLASAlloc;
    ComPtr<ID3D12Resource> m_TLAS;
    ComPtr<D3D12MA::Allocation> m_InstanceDescsAlloc;
    ComPtr<ID3D12Resource> m_InstanceDescs;
    ComPtr<D3D12MA::Allocation> m_ScratchResourceAlloc;
    ComPtr<ID3D12Resource> m_ScratchResource;

    ComPtr<ID3D12Resource> m_MissShaderTable;
    ComPtr<ID3D12Resource> m_HitGroupShaderTable;
    ComPtr<ID3D12Resource> m_RayGenShaderTable;

    ComPtr<ID3D12Resource> m_RaytracingOutput;
    u32 m_RaytracingOutputIndex = UINT_MAX;
    u32 m_RaytracingTlasIndex = UINT_MAX;

    ComPtr<ID3D12Resource> m_BlurTemp;
    ComPtr<ID3D12RootSignature> m_BlurRootSignature;
    ComPtr<ID3D12PipelineState> m_BlurHPso;
    ComPtr<ID3D12PipelineState> m_BlurVPso;

    u32 m_SrvRawIdx = UINT32_MAX;
    u32 m_SrvTempIdx = UINT32_MAX;
    u32 m_UavTempIdx = UINT32_MAX;

    Array<u32, IE_Constants::frameInFlightCount> m_HDRSrvIdx = {UINT32_MAX};

    Array<GBuffer, IE_Constants::frameInFlightCount> m_GBuffers = {};
    Array<Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>, IE_Constants::frameInFlightCount> m_GBuffersRTV = {};

    struct InFlightUpload
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<D3D12MA::Allocation> allocation;
    };
    Vector<InFlightUpload> m_InFlightUploads;
    Vector<ComPtr<ID3D12Resource>> m_Textures;

    float4x4 m_LastViewProjNoJ;

    // Cube map
    ComPtr<ID3D12Resource> m_EnvCubeMap;
    u32 m_EnvCubeMapSrvIdx = UINT32_MAX;
    ComPtr<ID3D12Resource> m_DiffuseIBL;
    u32 m_DiffuseIBLIdx = UINT32_MAX;
    ComPtr<ID3D12Resource> m_SpecularIBL;
    u32 m_SpecularIBLIdx = UINT32_MAX;
    ComPtr<ID3D12Resource> m_BrdfLUT;
    u32 m_BrdfLUTIdx = UINT32_MAX;

    // Depth pre pass
    ComPtr<ID3D12PipelineState> m_DepthPrePassOpaquePSO;
    ComPtr<ID3D12PipelineState> m_DepthPrePassOpaquePSO_NoCull;
    ComPtr<ID3D12RootSignature> m_DepthPrePassOpaqueRootSig;
    Array<ComPtr<D3D12MA::Allocation>, IE_Constants::frameInFlightCount> m_DSVsAllocations;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_DSVs;
    ComPtr<ID3D12DescriptorHeap> m_DSVsHeap;
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> m_DSVsHandle;
    Array<u32, IE_Constants::frameInFlightCount> m_DSVsIdx = {UINT32_MAX};
    Shader m_DepthPrePassAlphaTestShader;
    ComPtr<ID3D12PipelineState> m_DepthPrePassAlphaTestPSO;
    ComPtr<ID3D12PipelineState> m_DepthPrePassAlphaTestPSO_NoCull;
    ComPtr<ID3D12RootSignature> m_DepthPrePassAlphaTestRootSig;

    // GBuffer pass
    Shader m_AmplificationShader;
    Shader m_MeshShader;
    Array<Shader, AlphaMode_Count> m_PixelShader;

    // one face culled and the other double sided
    Array<Array<ComPtr<ID3D12PipelineState>, 2>, AlphaMode_Count> m_GBufferPassPSOs;
    Array<ComPtr<ID3D12RootSignature>, AlphaMode_Count> m_GBufferPassRootSigs;

    // Lighting pass
    ComPtr<ID3D12PipelineState> m_LightingPassPSO;
    ComPtr<ID3D12RootSignature> m_LightingPassRootSig;
    Shader m_LightingPassShader;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_HDR_RTVs;
    ComPtr<ID3D12DescriptorHeap> m_HDR_RTVsHeap;
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> m_HDR_RTVsHandle;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_LightingCBVs;
    Array<u8*, IE_Constants::frameInFlightCount> m_LightingCBVMapped;

    // Histogram pass
    u32 m_HistogramNumBuckets = 256;
    SharedPtr<Buffer> m_HistogramBuffer;
    Shader m_ClearUintShader;
    ComPtr<ID3D12RootSignature> m_ClearUintRootSig;
    ComPtr<ID3D12PipelineState> m_ClearUintPso;
    Shader m_HistogramShader;
    ComPtr<ID3D12RootSignature> m_HistogramRootSig;
    ComPtr<ID3D12PipelineState> m_HistogramPso;
    SharedPtr<Buffer> m_ExposureBuffer;
    Shader m_ExposureShader;
    ComPtr<ID3D12RootSignature> m_ExposureRootSig;
    ComPtr<ID3D12PipelineState> m_ExposurePso;
    SharedPtr<Buffer> m_AdaptExposureBuffer;
    Shader m_AdaptExposureShader;
    ComPtr<ID3D12RootSignature> m_AdaptExposureRootSig;
    ComPtr<ID3D12PipelineState> m_AdaptExposurePso;

    // Tone mapping pass
    ComPtr<ID3D12RootSignature> m_ToneMapRootSignature;
    ComPtr<ID3D12PipelineState> m_ToneMapPso;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_RTVs;
    ComPtr<ID3D12DescriptorHeap> m_RTVsHeap;
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> m_RTVsHandle;

    // FSR
    ffx::Context m_UpscalingContext;
    i32 m_JitterPhaseCount;
    i32 m_JitterIndex;
    f32 m_JitterX;
    f32 m_JitterY;
    uint2 m_RenderSize;
    uint2 m_PresentSize;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_FsrOutputs;
    Array<u32, IE_Constants::frameInFlightCount> m_FsrSrvIdx;

    // ImGui
    ComPtr<ID3D12DescriptorHeap> m_ImGuiSrvHeap;

    // SSAO
    ComPtr<ID3D12Resource> m_SSAOTexture;
    u32 m_SSAOUavIdx = UINT32_MAX;
    u32 m_SSAOSrvIdx = UINT32_MAX;
    ComPtr<ID3D12RootSignature> m_SSAORootSig;
    ComPtr<ID3D12PipelineState> m_SSAOPso;

    // Timings
    u64 m_TimestampFrequency = 0;
    struct TimDisp
    {
        const char* name;
        f64 ms;
    } m_LastGpuTimings[128];
    u32 m_LastGpuTimingCount = 0;
    struct TimingSmoother
    {
        const char* name = nullptr; // expected to be a stable literal
        double value = 0.0;
        bool initialized = false;
    };
    void UpdateGpuTimingAverages(float dtMs);
    TimingSmoother m_GpuTimingSmooth[128] = {};
    u32 m_GpuTimingSmoothCount = 0;
};

static void GPU_MARKER_BEGIN(const ComPtr<ID3D12GraphicsCommandList7>& cmd, Renderer::PerFrameData& frameData, const char* name)
{
    PIXBeginEvent(cmd.Get(), 0, name);

    u32 pi = frameData.gpuTimers.passCount++;
    frameData.gpuTimers.passes[pi].name = name;
    frameData.gpuTimers.passes[pi].idxBegin = frameData.gpuTimers.nextIdx++;
    cmd->EndQuery(frameData.gpuTimers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameData.gpuTimers.passes[pi].idxBegin);
}

static void GPU_MARKER_END(const ComPtr<ID3D12GraphicsCommandList7>& cmd, Renderer::PerFrameData& frameData)
{
    Renderer::GpuTimers::Pass& p = frameData.gpuTimers.passes[frameData.gpuTimers.passCount - 1];
    p.idxEnd = frameData.gpuTimers.nextIdx++;
    cmd->EndQuery(frameData.gpuTimers.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p.idxEnd);

    PIXEndEvent(cmd.Get());
}