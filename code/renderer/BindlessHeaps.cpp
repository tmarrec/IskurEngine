// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "BindlessHeaps.h"

void BindlessHeaps::Init(const ComPtr<ID3D12Device14>& device)
{
    m_Device = device;
    m_HandleSize[BindlessHeapType_CbvSrvUav] = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_HandleSize[BindlessHeapType_Sampler] = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    const auto createHeap = [&](BindlessHeapType heapType, const D3D12_DESCRIPTOR_HEAP_DESC& desc, const wchar_t* name) {
        IE_Check(m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_Heaps[heapType])));
        IE_Check(m_Heaps[heapType]->SetName(name));
        m_Capacity[heapType] = desc.NumDescriptors;
        m_DescriptorHeaps[heapType] = m_Heaps[heapType].Get();
    };

    constexpr D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 65536,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    createHeap(BindlessHeapType_CbvSrvUav, descriptorHeapDesc, L"CBV/SRV/UAV Heap");

    constexpr D3D12_DESCRIPTOR_HEAP_DESC samplerDescriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = 4080,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    createHeap(BindlessHeapType_Sampler, samplerDescriptorHeapDesc, L"Sampler Heap");
}

u32 BindlessHeaps::CreateSRV(const ComPtr<ID3D12Resource>& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    IE_Assert(m_NextIndex[BindlessHeapType_CbvSrvUav] < m_Capacity[BindlessHeapType_CbvSrvUav]);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Heaps[BindlessHeapType_CbvSrvUav]->GetCPUDescriptorHandleForHeapStart();
    const u32 index = m_NextIndex[BindlessHeapType_CbvSrvUav]++;
    handle.ptr += static_cast<size_t>(index) * m_HandleSize[BindlessHeapType_CbvSrvUav];
    m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, handle);
    return index;
}

u32 BindlessHeaps::CreateUAV(const ComPtr<ID3D12Resource>& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc)
{
    IE_Assert(m_NextIndex[BindlessHeapType_CbvSrvUav] < m_Capacity[BindlessHeapType_CbvSrvUav]);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Heaps[BindlessHeapType_CbvSrvUav]->GetCPUDescriptorHandleForHeapStart();
    const u32 index = m_NextIndex[BindlessHeapType_CbvSrvUav]++;
    handle.ptr += static_cast<size_t>(index) * m_HandleSize[BindlessHeapType_CbvSrvUav];
    m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, handle);
    return index;
}

u32 BindlessHeaps::CreateSampler(const D3D12_SAMPLER_DESC& samplerDesc)
{
    IE_Assert(m_NextIndex[BindlessHeapType_Sampler] < m_Capacity[BindlessHeapType_Sampler]);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Heaps[BindlessHeapType_Sampler]->GetCPUDescriptorHandleForHeapStart();
    const u32 index = m_NextIndex[BindlessHeapType_Sampler]++;
    handle.ptr += static_cast<size_t>(index) * m_HandleSize[BindlessHeapType_Sampler];
    m_Device->CreateSampler(&samplerDesc, handle);
    return index;
}

void BindlessHeaps::ResetAll()
{
    m_NextIndex.fill(0);
}

const Array<ID3D12DescriptorHeap*, BindlessHeapType_Count>& BindlessHeaps::GetDescriptorHeaps() const
{
    return m_DescriptorHeaps;
}

