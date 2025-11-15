// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <dxgi1_6.h>
#include <ffx_api.hpp>

#include "BindlessHeaps.h"
#include "Buffer.h"
#include "CPUGPU.h"
#include "Camera.h"
#include "CompileShader.h"
#include "Constants.h"
#include "GBuffer.h"
#include "GpuTimings.h"
#include "Primitive.h"
#include "Shader.h"

#include <common/IskurPackFormat.h>

class SceneLoader;

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

    BindlessHeaps& GetBindlessHeaps();
    const ComPtr<ID3D12Device14>& GetDevice() const;
    XMUINT2 GetRenderSize() const;

    struct PerFrameData
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList7> cmd;

        ComPtr<ID3D12Fence> frameFence;
        u64 frameFenceValue;

        GpuTimers gpuTimers;
    };
    PerFrameData& GetCurrentFrameData();

  private:
    friend class SceneLoader;

    struct DepthPreResources
    {
        Array<ComPtr<ID3D12PipelineState>, CullMode_Count> opaquePSO;
        ComPtr<ID3D12RootSignature> opaqueRootSig;
        Array<ComPtr<ID3D12PipelineState>, CullMode_Count> alphaTestPSO;
        ComPtr<ID3D12RootSignature> alphaTestRootSig;
        Array<ComPtr<D3D12MA::Allocation>, IE_Constants::frameInFlightCount> dsvAllocs;
        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> dsvs;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> dsvHandles;
        Array<u32, IE_Constants::frameInFlightCount> dsvSrvIdx = {UINT32_MAX};
        Shader alphaTestShader;
    } m_DepthPre{};

    struct GBufferResources
    {
        Shader amplificationShader;
        Shader meshShader;
        Array<Shader, AlphaMode_Count> pixelShaders;

        Array<GBuffer, IE_Constants::frameInFlightCount> gbuffers = {};
        Array<Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>, IE_Constants::frameInFlightCount> rtvHandles = {};

        Array<Array<ComPtr<ID3D12PipelineState>, CullMode_Count>, AlphaMode_Count> psos;
        Array<ComPtr<ID3D12RootSignature>, AlphaMode_Count> rootSigs;
    } m_GBuf{};

    struct LightingResources
    {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rootSig;
        Shader shader;

        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> hdrRt;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> rtvHandles;

        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> cb;
        Array<u8*, IE_Constants::frameInFlightCount> cbMapped;
    } m_Light{};

    struct HistogramResources
    {
        u32 numBuckets = 256;
        SharedPtr<Buffer> histogramBuffer;

        Shader clearUintShader;
        ComPtr<ID3D12RootSignature> clearUintRootSig;
        ComPtr<ID3D12PipelineState> clearUintPso;

        Shader histogramShader;
        ComPtr<ID3D12RootSignature> histogramRootSig;
        ComPtr<ID3D12PipelineState> histogramPso;

        SharedPtr<Buffer> exposureBuffer;
        Shader exposureShader;
        ComPtr<ID3D12RootSignature> exposureRootSig;
        ComPtr<ID3D12PipelineState> exposurePso;

        SharedPtr<Buffer> adaptExposureBuffer;
        Shader adaptExposureShader;
        ComPtr<ID3D12RootSignature> adaptExposureRootSig;
        ComPtr<ID3D12PipelineState> adaptExposurePso;
    } m_Histo{};

    struct ToneMapResources
    {
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;

        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> sdrRt;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Array<D3D12_CPU_DESCRIPTOR_HANDLE, IE_Constants::frameInFlightCount> rtvHandles;
    } m_Tonemap{};

    struct FsrResources
    {
        ffx::Context context;
        i32 jitterPhaseCount = 0;
        i32 jitterIndex = 0;
        f32 jitterX = 0.0f;
        f32 jitterY = 0.0f;
        XMUINT2 renderSize{};
        XMUINT2 presentSize{};
        Array<ComPtr<ID3D12Resource>, IE_Constants::frameInFlightCount> outputs;
        Array<u32, IE_Constants::frameInFlightCount> srvIdx;
        Array<D3D12_RESOURCE_STATES, IE_Constants::frameInFlightCount> outputState = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    } m_Fsr{};

    struct SsaoResources
    {
        ComPtr<ID3D12Resource> texture;
        u32 uavIdx = UINT32_MAX;
        u32 srvIdx = UINT32_MAX;
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;
    } m_Ssao{};

    // Env map / IBL resources grouped in a single struct
    struct EnvMapResources
    {
        WString name;
        ComPtr<ID3D12Resource> envCube;
        u32 envSrvIdx = UINT32_MAX;

        ComPtr<ID3D12Resource> diffuseIBL;
        u32 diffuseSrvIdx = UINT32_MAX;

        ComPtr<ID3D12Resource> specularIBL;
        u32 specularSrvIdx = UINT32_MAX;

        ComPtr<ID3D12Resource> brdfLut;
        u32 brdfSrvIdx = UINT32_MAX;
    } m_Env{};

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
    void CreateEnvMapResources(const WString& envName); // <— NEW

    void LoadScene();

    static void WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue);

    void BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY);
    void Pass_DepthPre(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Raytracing(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_SSAO(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_Lighting(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_FSR(const ComPtr<ID3D12GraphicsCommandList7>& cmd, f32 jitterNormX, f32 jitterNormY, const Camera::FrameData& cameraFrameData);
    void Pass_Histogram(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Tonemap(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_ImGui(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void EndFrame(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);

    ComPtr<ID3D12Device14> m_Device;
    ComPtr<ID3D12Debug> m_Debug;
    ComPtr<IDXGIFactory6> m_DxgiFactory;
    ComPtr<D3D12MA::Allocator> m_Allocator;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain4> m_Swapchain;

    BindlessHeaps m_BindlessHeaps;

    Array<PerFrameData, IE_Constants::frameInFlightCount> m_AllFrameData{};

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

    // Frame timing
    GpuTimingState m_GpuTimingState{};

    // Bindless indices maps
    Vector<u32> m_TxhdToSrv;  // TXHD index -> SRV index
    Vector<u32> m_SampToHeap; // SAMP index -> sampler heap index

    // Scene
    Vector<Primitive> m_Primitives;

    struct PrimitiveRenderData
    {
        u32 primIndex;
        PrimitiveConstants primConstants;
    };
    Vector<PrimitiveRenderData> m_PrimitivesRenderData[AlphaMode_Count][CullMode_Count];

    Vector<ComPtr<ID3D12Resource>> m_Textures;

    struct UploadTemp
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<D3D12MA::Allocation> allocation;
    };
    Vector<UploadTemp> m_InFlightUploads;

    // ImGui
    ComPtr<ID3D12DescriptorHeap> m_ImGuiSrvHeap;
};
