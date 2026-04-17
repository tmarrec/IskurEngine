// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <dxgi1_6.h>

#include "Buffer.h"
#include "Constants.h"
#include "Timings.h"

class BindlessHeaps;
class Window;

class RenderDevice
{
  public:
    struct PerFrameData
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList7> cmd;

        ComPtr<ID3D12Fence> frameFence;
        u64 frameFenceValue = 0;
        HANDLE fenceEvent = nullptr;

        GpuTimers gpuTimers;
    };

    struct UploadTemp
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<D3D12MA::Allocation> allocation;
    };

    void Init(const Window& window, const XMUINT2& presentSize);
    void Terminate();

    SharedPtr<Buffer> CreateBuffer(BindlessHeaps& bindlessHeaps, ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc);
    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    void CreateGPUTimers(TimingState& timingState);
    void WaitForGpuIdle();
    void WaitForFrame(PerFrameData& frameData);
    void ExecuteFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd, u32 streamlineFrameIndex);
    void ExecuteAndWait(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void TrackUpload(UploadTemp&& upload);
    void ClearTrackedUploads(u32 frameInFlightIdx);
    void ClearAllTrackedUploads();

    u32 GetCurrentBackBufferIndex() const;
    PerFrameData& GetCurrentFrameData();
    Array<PerFrameData, IE_Constants::frameInFlightCount>& GetAllFrameData();

    const ComPtr<ID3D12Device14>& GetDevice() const;
    const ComPtr<D3D12MA::Allocator>& GetAllocator() const;
    const ComPtr<ID3D12CommandQueue>& GetCommandQueue() const;
    const ComPtr<IDXGISwapChain4>& GetSwapchain() const;

  private:
    static void WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue, HANDLE& fenceEvent);

    void CreateDevice();
    void CreateAllocator();
    void CreateCommandQueue();
    void CreateSwapchain(const Window& window, const XMUINT2& presentSize);
    void CreateFrameSynchronizationFences();
    void CreateCommands();

    ComPtr<ID3D12Device14> m_Device;
    ComPtr<ID3D12Device14> m_DeviceProxy;
    ComPtr<ID3D12Debug> m_Debug;
    ComPtr<IDXGIFactory6> m_DxgiFactory;
    ComPtr<IDXGIFactory6> m_DxgiFactoryProxy;
    ComPtr<D3D12MA::Allocator> m_Allocator;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain4> m_Swapchain;

    Array<PerFrameData, IE_Constants::frameInFlightCount> m_AllFrameData{};
    Array<Vector<UploadTemp>, IE_Constants::frameInFlightCount> m_InFlightUploads{};
};
