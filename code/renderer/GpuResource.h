// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>

#include <d3dx12/d3dx12.h>

#include "common/Asserts.h"
#include "common/Types.h"

struct GpuResource
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource* Get() const
    {
        return resource.Get();
    }

    ID3D12Resource* operator->() const
    {
        IE_Assert(resource != nullptr);
        return resource.Get();
    }

    explicit operator bool() const
    {
        return resource != nullptr;
    }

    void Reset(D3D12_RESOURCE_STATES newState = D3D12_RESOURCE_STATE_COMMON)
    {
        resource.Reset();
        state = newState;
    }

    void SetName(const wchar_t* name) const
    {
        IE_Assert(resource != nullptr);
        IE_Check(resource->SetName(name));
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const
    {
        IE_Assert(resource != nullptr);
        return resource->GetGPUVirtualAddress();
    }

    D3D12_RESOURCE_DESC GetDesc() const
    {
        IE_Assert(resource != nullptr);
        return resource->GetDesc();
    }

    HRESULT Map(UINT subresource, const D3D12_RANGE* readRange, void** data) const
    {
        IE_Assert(resource != nullptr);
        return resource->Map(subresource, readRange, data);
    }

    void Unmap(UINT subresource, const D3D12_RANGE* writtenRange) const
    {
        IE_Assert(resource != nullptr);
        resource->Unmap(subresource, writtenRange);
    }

    void Transition(const ComPtr<ID3D12GraphicsCommandList7>& cmd, D3D12_RESOURCE_STATES newState)
    {
        IE_Assert(resource != nullptr);
        if (state == newState)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), state, newState);
        cmd->ResourceBarrier(1, &barrier);
        state = newState;
    }

    void UavBarrier(const ComPtr<ID3D12GraphicsCommandList7>& cmd) const
    {
        IE_Assert(resource != nullptr);
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource.Get());
        cmd->ResourceBarrier(1, &barrier);
    }
};
