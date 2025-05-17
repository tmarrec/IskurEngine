// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "Buffer.h"
#include "Constants.h"
#include "Material.h"
#include "PipelineState.h"
#include "RenderTarget.h"
#include "Shader.h"
#include "World.h"

#include "../common/Singleton.h"

namespace RtRootSignatureParams
{
enum Value : u8
{
    OutputViewSlot = 0,
    AccelerationStructureSlot = 1,
    ConstantsSlot = 2,
    DepthBufferSlot = 3,
    SamplerSlot = 4,
    Count
};
} // namespace RtRootSignatureParams

struct RtRootConstants
{
    float4x4 invViewProj;

    f32 zNear;
    f32 zFar;
    f32 pad0;
    u32 resolutionType;

    float3 sunDir;
    u32 frameIndex;

    float3 cameraPos;
    f32 pad1;
};

struct PrimitiveRootConstants
{
    float4x4 world;

    u32 meshletCount;
    u32 materialIdx;
    u32 verticesBufferIndex;
    u32 meshletsBufferIndex;

    u32 meshletVerticesBufferIndex;
    u32 meshletTrianglesBufferIndex;
    u32 meshletBoundsBufferIndex;
    u32 materialsBufferIndex;
};

struct alignas(256) GlobalConstants
{
    float4x4 proj;
    float4x4 view;

    float3 cameraPos;
    u32 unused;

    float4 planes[6];

    u32 raytracingOutputIndex;
    float3 sunDir;
};

class Renderer : public Singleton<Renderer>
{
  public:
    void Init();

    void Render();

    SharedPtr<PipelineState> CreatePipelineState(const SharedPtr<Shader>& amplificationShader, const SharedPtr<Shader>& meshShader, const SharedPtr<Shader>& pixelShader,
                                                 Vector<SharedPtr<RenderTarget>> renderTargets, const SharedPtr<RenderTarget>& depthStencil) const;

    void GetOrWaitNextFrame();

    SharedPtr<Buffer> CreateStructuredBuffer(u32 sizeInBytes, u32 strideInBytes, const WString& name, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT);
    SharedPtr<Buffer> CreateBytesBuffer(u32 numElements, const WString& name, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT);
    void UploadTexture(const ComPtr<ID3D12GraphicsCommandList7>& cmdList, i32 width, i32 height, const void* data, u32 sizeInBytes, DXGI_FORMAT format);

    D3D12_CPU_DESCRIPTOR_HANDLE GetNewCBVSRVUAVHeapHandle();

    static SharedPtr<Shader> LoadShader(ShaderType type, const WString& filename);

    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmdList, const SharedPtr<Buffer>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0) const;
    static void SetResourceBufferData(const ComPtr<ID3D12Resource>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    static void Barrier(const ComPtr<ID3D12GraphicsCommandList7>& commandList, const ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);
    static void UAVBarrier(const ComPtr<ID3D12GraphicsCommandList7>& commandList, const ComPtr<ID3D12Resource>& resource);

    void AllocateUploadBuffer(const void* pData, u32 sizeInBytes, u32 offsetInBytes, ComPtr<ID3D12Resource>& resource, const wchar_t* resourceName) const;
    void AllocateUAVBuffer(u32 sizeInBytes, ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES initialResourceState, const wchar_t* resourceName) const;

  private:
    void SetupRaytracing(ComPtr<ID3D12GraphicsCommandList7>& cmdList);

    ComPtr<ID3D12Device14> m_Device;
    ComPtr<ID3D12Debug> m_Debug;
    ComPtr<IDXGIFactory6> m_DxgiFactory;
    ComPtr<D3D12MA::Allocator> m_Allocator;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain4> m_Swapchain;

    HANDLE m_FenceEvent = nullptr;

    struct PerFrameData
    {
        ComPtr<ID3D12Fence> renderFinishedFence;
        u64 renderFinishedFenceValue;
        Array<ComPtr<ID3D12CommandAllocator>, AlphaMode_Count> commandAllocators;
        ComPtr<ID3D12CommandAllocator> rtCommandAllocator;
        ComPtr<ID3D12CommandAllocator> depthCommandAllocator;
    };
    Array<PerFrameData, IE_Constants::frameInFlightCount> m_AllFrameData{};
    PerFrameData* m_Pfd = nullptr;

    u32 m_FrameInFlightIdx = 0;
    u32 m_FrameIndex = 0;

    SharedPtr<RenderTarget> m_ColorTarget;
    SharedPtr<RenderTarget> m_DepthStencil;
    SharedPtr<Shader> m_AmplificationShader;
    SharedPtr<Shader> m_MeshShader;
    SharedPtr<Shader> m_PixelShader;

    ComPtr<ID3D12Resource> m_ConstantsBuffer;
    GlobalConstants m_Constants{};

    SharedPtr<PipelineState> m_PipelineStates;

    D3D12_VIEWPORT m_Viewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_Rect = {0, 0, 0, 0};

    ComPtr<ID3D12DescriptorHeap> m_CBVSRVUAVHeap;
    u32 m_CBVSRVUAVHeapLastIdx = 0;

    ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;
    u32 m_SamplerHeapLastIdx = 0;

    D3D12_GPU_DESCRIPTOR_HANDLE m_DepthResourceSRVGpuDescriptor{};

    Vector<Material> m_Materials;
    SharedPtr<Buffer> m_MaterialsBuffer;

    SharedPtr<World> m_World;

    Array<Vector<SharedPtr<Primitive>>, AlphaMode_Count> m_PrimitivesPerAlphaMode; // not really safe but good enough for now
    Vector<SharedPtr<Primitive>> m_Primitives;                                     // not really safe but good enough for now

    ComPtr<ID3D12RootSignature> m_RaytracingGlobalRootSignature;
    ComPtr<ID3D12StateObject> m_DxrStateObject;

    ComPtr<ID3D12Resource> m_TLAS;
    ComPtr<ID3D12Resource> m_InstanceDescs;
    ComPtr<ID3D12Resource> m_ScratchResource;

    ComPtr<ID3D12Resource> m_MissShaderTable;
    ComPtr<ID3D12Resource> m_HitGroupShaderTable;
    ComPtr<ID3D12Resource> m_RayGenShaderTable;

    ComPtr<ID3D12DescriptorHeap> m_SamplerDepthRTHeap;

    // Raytracing output
    ComPtr<ID3D12Resource> m_RaytracingOutput;
    D3D12_GPU_DESCRIPTOR_HANDLE m_RaytracingOutputResourceUAVGpuDescriptor{};
    u32 m_RaytracingOutputIndex = UINT_MAX;
};
