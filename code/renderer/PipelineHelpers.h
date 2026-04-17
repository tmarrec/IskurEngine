// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <d3dx12/d3dx12.h>

#include "Shader.h"
#include "common/Asserts.h"

namespace PipelineHelpers
{
inline void CreateComputePipeline(const ComPtr<ID3D12Device14>& device, const SharedPtr<Shader>& shader, ComPtr<ID3D12RootSignature>& outRootSig, ComPtr<ID3D12PipelineState>& outPso)
{
    IE_Assert(shader && shader->IsValid());
    outRootSig = shader->GetOrCreateRootSignature(device);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = outRootSig.Get();
    psoDesc.CS = shader->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso)));
}

inline void CreateFullscreenGraphicsPipeline(const ComPtr<ID3D12Device14>& device, const SharedPtr<Shader>& vertexShader, const SharedPtr<Shader>& pixelShader, DXGI_FORMAT rtvFormat,
                                             ComPtr<ID3D12RootSignature>& outRootSig, ComPtr<ID3D12PipelineState>& outPso)
{
    IE_Assert(vertexShader && vertexShader->IsValid());
    IE_Assert(pixelShader && pixelShader->IsValid());
    outRootSig = pixelShader->GetOrCreateRootSignature(device);

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = outRootSig.Get();
    psoDesc.VS = vertexShader->GetBytecode();
    psoDesc.PS = pixelShader->GetBytecode();
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.SampleDesc = DefaultSampleDesc();
    IE_Check(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso)));
}
} // namespace PipelineHelpers
