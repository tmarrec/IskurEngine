// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

struct Buffer
{
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> buffer;
    u32 srvIndex = UINT32_MAX;
    u32 uavIndex = UINT32_MAX;
    u32 numElements = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct BufferCreateDesc
{
    enum class ViewKind : u8
    {
        None,
        Structured,
        Raw,
    };

    // Resource
    u32 sizeInBytes = 0;
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;

    // Views
    ViewKind viewKind = ViewKind::None;
    bool createSRV = false;
    bool createUAV = false;

    // Structured view params
    u32 strideInBytes = 0;

    // Optional init data (copied at creation)
    const void* initialData = nullptr;
    u32 initialDataSize = 0;

    // States
    // - initialState: used when there is NO initialData (and cmd is provided) to transition after creation
    // - finalState: used when there IS initialData (after copy) to transition to final
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;

    // Debug
    const wchar_t* name = L"Buffer";
};