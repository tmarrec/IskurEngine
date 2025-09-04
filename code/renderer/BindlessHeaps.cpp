// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "BindlessHeaps.h"

#include "common/Asserts.h"

void BindlessHeaps::Init(const ComPtr<ID3D12Device14>& device)
{
    m_Device = device;
    m_CbvSrvUavHandleSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_SamplerHandleSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    constexpr D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 65536,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_CbvSrvUavHeap)));
    IE_Check(m_CbvSrvUavHeap->SetName(L"CBV/SRV/UAV Heap"));

    constexpr D3D12_DESCRIPTOR_HEAP_DESC samplerDescriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = 4080,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&samplerDescriptorHeapDesc, IID_PPV_ARGS(&m_SamplerHeap)));
    IE_Check(m_SamplerHeap->SetName(L"Sampler Heap"));
}

u32 BindlessHeaps::CreateSRV(ComPtr<ID3D12Resource> resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_CbvSrvUavNextIndex * m_CbvSrvUavHandleSize;
    m_CbvSrvUavNextIndex++;
    m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, handle);
    return m_CbvSrvUavNextIndex - 1;
}

u32 BindlessHeaps::CreateUAV(ComPtr<ID3D12Resource> resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_CbvSrvUavNextIndex * m_CbvSrvUavHandleSize;
    m_CbvSrvUavNextIndex++;
    m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, handle);
    return m_CbvSrvUavNextIndex - 1;
}

u32 BindlessHeaps::CreateSampler(const D3D12_SAMPLER_DESC& samplerDesc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_SamplerNextIndex * m_SamplerHandleSize;
    m_SamplerNextIndex++;
    m_Device->CreateSampler(&samplerDesc, handle);
    return m_SamplerNextIndex - 1;
}