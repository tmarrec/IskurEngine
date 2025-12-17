// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

struct GBuffer
{
    static constexpr u32 targetCount = 6;

    ComPtr<ID3D12Resource> albedo;
    ComPtr<ID3D12Resource> normal;
    ComPtr<ID3D12Resource> normalGeo;
    ComPtr<ID3D12Resource> material;
    ComPtr<ID3D12Resource> motionVector;
    ComPtr<ID3D12Resource> ao;

    u32 albedoIndex = UINT32_MAX;
    u32 normalIndex = UINT32_MAX;
    u32 normalGeoIndex = UINT32_MAX;
    u32 materialIndex = UINT32_MAX;
    u32 motionVectorIndex = UINT32_MAX;
    u32 aoIndex = UINT32_MAX;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
};
