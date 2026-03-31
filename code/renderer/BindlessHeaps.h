// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

enum BindlessHeapType : u32
{
    BindlessHeapType_CbvSrvUav = 0,
    BindlessHeapType_Sampler,
    BindlessHeapType_Count,
};

class BindlessHeaps
{
  public:
    void Init(const ComPtr<ID3D12Device14>& device);

    u32 CreateSRV(const ComPtr<ID3D12Resource>& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
    u32 CreateUAV(const ComPtr<ID3D12Resource>& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc);
    u32 CreateSampler(const D3D12_SAMPLER_DESC& samplerDesc);
    void ResetAll();

    const Array<ID3D12DescriptorHeap*, BindlessHeapType_Count>& GetDescriptorHeaps() const;

  private:
    ComPtr<ID3D12Device14> m_Device;

    Array<u32, BindlessHeapType_Count> m_NextIndex{};
    Array<u32, BindlessHeapType_Count> m_Capacity{};
    Array<u32, BindlessHeapType_Count> m_HandleSize{};
    Array<ComPtr<ID3D12DescriptorHeap>, BindlessHeapType_Count> m_Heaps{};
    Array<ID3D12DescriptorHeap*, BindlessHeapType_Count> m_DescriptorHeaps{};
};

