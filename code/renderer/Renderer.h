// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <ffx_api.hpp>
#include <pix.h>

#include "BindlessHeaps.h"
#include "Buffer.h"
#include "CPUGPU.h"
#include "CompileShader.h"
#include "Constants.h"
#include "GpuTimings.h"
#include "Shader.h"

#include "common/Singleton.h"

#include <common/IskurPackFormat.h>

enum AlphaMode : u32
{
    AlphaMode_Opaque,
    AlphaMode_Blend,
    AlphaMode_Mask,

    AlphaMode_Count,
};

enum CullMode : u32
{
    CullMode_Back,
    CullMode_None,

    CullMode_Count
};

struct GBuffer
{
    static constexpr u32 targetCount = 5;

    ComPtr<ID3D12Resource> albedo;
    ComPtr<ID3D12Resource> normal;
    ComPtr<ID3D12Resource> material;
    ComPtr<ID3D12Resource> motionVector;
    ComPtr<ID3D12Resource> ao;

    u32 albedoIndex = UINT32_MAX;
    u32 normalIndex = UINT32_MAX;
    u32 materialIndex = UINT32_MAX;
    u32 motionVectorIndex = UINT32_MAX;
    u32 aoIndex = UINT32_MAX;

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

    void CreateFSRPassResources();
    void CreateDepthPrePassResources(const Vector<WString>& globalDefines);
    void CreateGBufferPassResources(const Vector<WString>& globalDefines);
    void CreateLightingPassResources(const Vector<WString>& globalDefines);
    void CreateHistogramPassResources(const Vector<WString>& globalDefines);
    void CreateToneMapPassResources(const Vector<WString>& globalDefines);
    void CreateSSAOResources(const Vector<WString>& globalDefines);

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

    XMFLOAT3 m_SunDir = {0.3f, 1.f, 0.75f};

    Vector<Material> m_Materials;
    SharedPtr<Buffer> m_MaterialsBuffer;

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

    Array<GBuffer, IE_Constants::frameInFlightCount> m_GBuffers = {};
    Array<Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>, IE_Constants::frameInFlightCount> m_GBuffersRTV = {};

    Array<D3D12_RESOURCE_STATES, IE_Constants::frameInFlightCount> m_FsrOutputState = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};

    struct InFlightUpload
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<D3D12MA::Allocation> allocation;
    };
    Vector<InFlightUpload> m_InFlightUploads;
    Vector<ComPtr<ID3D12Resource>> m_Textures;

    XMFLOAT4X4 m_LastViewProjNoJ;

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
    Array<ComPtr<ID3D12PipelineState>, CullMode_Count> m_DepthPrePassOpaquePSO;
    ComPtr<ID3D12RootSignature> m_DepthPrePassOpaqueRootSig;
    Array<ComPtr<ID3D12PipelineState>, CullMode_Count> m_DepthPrePassAlphaTestPSO;
    ComPtr<ID3D12RootSignature> m_DepthPrePassAlphaTestRootSig;
    Array<ComPtr<D3D12MA::Allocation>, IE_Constants::frameInFlightCount> m_DSVsAllocations;
    Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> m_DSVs;
    ComPtr<ID3D12DescriptorHeap> m_DSVsHeap;
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> m_DSVsHandle;
    Array<u32, IE_Constants::frameInFlightCount> m_DSVsIdx = {UINT32_MAX};
    Shader m_DepthPrePassAlphaTestShader;

    // GBuffer pass
    Shader m_AmplificationShader;
    Shader m_MeshShader;
    Array<Shader, AlphaMode_Count> m_PixelShader;

    Array<Array<ComPtr<ID3D12PipelineState>, CullMode_Count>, AlphaMode_Count> m_GBufferPassPSOs;
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
    i32 m_JitterPhaseCount = 0;
    i32 m_JitterIndex = 0;
    f32 m_JitterX = 0.0f;
    f32 m_JitterY = 0.0f;
    XMUINT2 m_RenderSize;
    XMUINT2 m_PresentSize;
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

    GpuTimingState m_GpuTimingState{};

    //
    Vector<u32> m_TxhdToSrv;  // TXHD index -> SRV index
    Vector<u32> m_SampToHeap; // SAMP index -> sampler heap index

    struct GpuPrim
    {
        u32 materialIdx = 0;

        // GPU buffers (bindless indices)
        SharedPtr<Buffer> vertices; // stride sizeof(Vertex)
        SharedPtr<Buffer> meshlets; // bytes
        SharedPtr<Buffer> mlVerts;  // u32
        SharedPtr<Buffer> mlTris;   // bytes
        SharedPtr<Buffer> mlBounds; // sizeof(meshopt_Bounds)

        u32 meshletCount = 0;

        // (For ray tracing build)
        const Vertex* cpuVertices = nullptr;
        u32 vertexCount = 0;
        const u32* cpuIndices = nullptr;
        u32 indexCount = 0;

        // BLAS resources
        ComPtr<ID3D12Resource> blas;
        ComPtr<D3D12MA::Allocation> blasAlloc;
        ComPtr<ID3D12Resource> scratch;
        ComPtr<D3D12MA::Allocation> scratchAlloc;

        // temp upload for RT (optional to keep around)
        ComPtr<ID3D12Resource> rtVB;
        ComPtr<D3D12MA::Allocation> rtVBAlloc;
        ComPtr<ID3D12Resource> rtIB;
        ComPtr<D3D12MA::Allocation> rtIBAlloc;
    };

    Vector<GpuPrim> m_Primitives;
    Vector<IEPack::DrawItem> m_Draw[AlphaMode_Count][CullMode_Count];
};
