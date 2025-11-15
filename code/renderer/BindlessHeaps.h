// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class BindlessHeaps
{
  public:
    void Init(const ComPtr<ID3D12Device14>& device);

    u32 CreateSRV(const ComPtr<ID3D12Resource>& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
    u32 CreateUAV(const ComPtr<ID3D12Resource>& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc);
    u32 CreateSampler(const D3D12_SAMPLER_DESC& samplerDesc);

    Array<ID3D12DescriptorHeap*, 2> GetDescriptorHeaps() const;

  private:
    ComPtr<ID3D12Device14> m_Device;

    u32 m_CbvSrvUavNextIndex = 0;
    u32 m_SamplerNextIndex = 0;

    u32 m_CbvSrvUavHandleSize = 0;
    u32 m_SamplerHandleSize = 0;

    ComPtr<ID3D12DescriptorHeap> m_CbvSrvUavHeap;
    ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;
};