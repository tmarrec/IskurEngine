// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Primitive.h"

class Raytracing : public Singleton<Raytracing>
{
  public:
    struct RTInstance
    {
        u32 primIndex;
        XMFLOAT4X4 world;
    };

    void Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances);
    void Terminate();

    void InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances);
    void InitRaytracingShadows();

    struct ShadowPassInput
    {
        u32 depthTextureIndex;
        u32 depthSamplerIndex;
        XMFLOAT3 sunDir;
        u32 frameIndex;
    };
    void ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ShadowPassInput& input);

    struct ShadowPassOutput
    {
        ComPtr<ID3D12Resource> resource;
        u32 uavIndex;
        u32 srvIndex;
    };
    const ShadowPassOutput& GetShadowPassOutput() const;

  private:
    ShadowPassOutput m_ShadowPassOutput = {};

    ComPtr<ID3D12RootSignature> m_RaytracingGlobalRootSignature;
    ComPtr<ID3D12StateObject> m_DxrStateObject;

    ComPtr<D3D12MA::Allocation> m_TLASAlloc;
    ComPtr<ID3D12Resource> m_TLAS;
    ComPtr<D3D12MA::Allocation> m_InstanceDescsAlloc;
    ComPtr<ID3D12Resource> m_InstanceDescs;
    ComPtr<D3D12MA::Allocation> m_ScratchResourceAlloc;
    ComPtr<ID3D12Resource> m_ScratchResource;

    ComPtr<ID3D12Resource> m_MissShaderTable;
    ComPtr<ID3D12Resource> m_HitGroupShaderTable;
    ComPtr<ID3D12Resource> m_RayGenShaderTable;

    struct Blur
    {
        ComPtr<ID3D12Resource> intermediateResource;
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> horizontalPso;
        ComPtr<ID3D12PipelineState> verticalPso;

        u32 srvRawIdx = UINT32_MAX;
        u32 srvIntermediateIdx = UINT32_MAX;
        u32 uavIntermediateIdx = UINT32_MAX;
    };
    Blur m_Blur;

    RtShadowsTraceConstants m_Constants = {};
    D3D12_DISPATCH_RAYS_DESC m_ShadowPassDispatchRaysDesc = {};
};
