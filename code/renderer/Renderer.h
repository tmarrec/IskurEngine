// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <dxgi1_6.h>

#include "BindlessHeaps.h"
#include "Buffer.h"
#include "Camera.h"
#include "Constants.h"
#include "Environments.h"
#include "GBuffer.h"
#include "GpuTimings.h"
#include "Primitive.h"
#include "Shader.h"
#include "common/IskurPackFormat.h"
#include "shaders/CPUGPU.h"

class SceneLoader;

enum AlphaMode : u8
{
    AlphaMode_Opaque,
    AlphaMode_Blend,
    AlphaMode_Mask,

    AlphaMode_Count,
};

enum CullMode : u8
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

    SharedPtr<Buffer> CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc);
    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    static void Barrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);
    static void UAVBarrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ComPtr<ID3D12Resource>& resource);

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

    const Environment& GetCurrentEnvironment() const;

    void RequestShaderReload();

  private:
    friend class SceneLoader;

    SharedPtr<Buffer> m_MaterialsBuffer;

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
        SharedPtr<Shader> alphaTestShader;
    } m_DepthPre{};

    struct GBufferResources
    {
        SharedPtr<Shader> amplificationShader;
        SharedPtr<Shader> meshShader;
        Array<SharedPtr<Shader>, AlphaMode_Count> pixelShaders;

        Array<GBuffer, IE_Constants::frameInFlightCount> gbuffers = {};
        Array<Array<D3D12_CPU_DESCRIPTOR_HANDLE, GBuffer::targetCount>, IE_Constants::frameInFlightCount> rtvHandles = {};

        Array<Array<ComPtr<ID3D12PipelineState>, CullMode_Count>, AlphaMode_Count> psos;
        Array<ComPtr<ID3D12RootSignature>, AlphaMode_Count> rootSigs;
    } m_GBuf{};

    struct LightingResources
    {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rootSig;
        SharedPtr<Shader> pxShader;
        SharedPtr<Shader> vxShader;

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

        SharedPtr<Shader> clearUintShader;
        ComPtr<ID3D12RootSignature> clearUintRootSig;
        ComPtr<ID3D12PipelineState> clearUintPso;

        SharedPtr<Shader> histogramShader;
        ComPtr<ID3D12RootSignature> histogramRootSig;
        ComPtr<ID3D12PipelineState> histogramPso;

        SharedPtr<Buffer> exposureBuffer;
        SharedPtr<Shader> exposureShader;
        ComPtr<ID3D12RootSignature> exposureRootSig;
        ComPtr<ID3D12PipelineState> exposurePso;

        SharedPtr<Buffer> adaptExposureBuffer;
        SharedPtr<Shader> adaptExposureShader;
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
        SharedPtr<Shader> vxShader;
        SharedPtr<Shader> pxShader;
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
        Array<D3D12_RESOURCE_STATES, IE_Constants::frameInFlightCount> outputState;
    } m_Fsr{};

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
    void CreateGBufferPassResources();
    void CreateLightingPassResources();
    void CreateHistogramPassResources();

    void CreateDepthPrePassPipelines(const Vector<WString>& globalDefines);
    void CreateGBufferPassPipelines(const Vector<WString>& globalDefines);
    void CreateLightingPassPipelines(const Vector<WString>& globalDefines);
    void CreateHistogramPassPipelines(const Vector<WString>& globalDefines);
    void CreateToneMapPassPipelines(const Vector<WString>& globalDefines);

    void LoadScene();

    static void WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue);

    void BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY);
    void Pass_DepthPre(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Shadows(const ComPtr<ID3D12GraphicsCommandList7>& cmd) const;
    void Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_PathTrace(const ComPtr<ID3D12GraphicsCommandList7>& cmd) const;
    void Pass_Lighting(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_FSR(const ComPtr<ID3D12GraphicsCommandList7>& cmd, f32 jitterNormX, f32 jitterNormY, const Camera::FrameData& cameraFrameData);
    void Pass_Histogram(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Tonemap(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_ImGui(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void EndFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);

    void WaitForGpuIdle();
    void ReloadShaders();

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

    SharedPtr<Buffer> m_ConstantsBuffer;
    u8* m_ConstantsCbMapped = nullptr;
    u32 m_ConstantsCbStride = 0;

    D3D12_VIEWPORT m_RenderViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_RenderRect = {0, 0, 0, 0};
    D3D12_VIEWPORT m_PresentViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_PresentRect = {0, 0, 0, 0};

    Vector<Material> m_Materials;

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
    Array<Vector<UploadTemp>, IE_Constants::frameInFlightCount> m_InFlightUploads;

    // ImGui
    ComPtr<ID3D12DescriptorHeap> m_ImGuiSrvHeap;

    Environments m_Environments;

    bool m_PendingShaderReload = false;
};
