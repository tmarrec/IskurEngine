// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <dxgi1_6.h>

#include "BindlessHeaps.h"
#include "Buffer.h"
#include "GpuResource.h"
#include "Shader.h"
#include "Timings.h"
#include "Texture.h"
#include "common/Types.h"

class RenderDevice;

class AutoExposure
{
  public:
    void CreateResources(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps);
    void CreatePipelines(const ComPtr<ID3D12Device14>& device, const Vector<String>& globalDefines);
    void InvalidateDescriptorIndices();

    void Pass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const BindlessHeaps& bindlessHeaps, GpuResource& hdrTexture, u32 hdrSrvIndex, u32 depthSrvIndex,
              const XMUINT2& renderSize, f32 frameTimeMs);

    u32 GetFinalExposureTextureSrvIndex() const;
    const Texture& GetFinalExposureTexture() const;

  private:
    struct Resources
    {
        u32 numBuckets = 256;
        SharedPtr<Buffer> histogramBuffer;

        SharedPtr<Shader> clearUintShader;
        ComPtr<ID3D12RootSignature> clearUintRootSig;
        ComPtr<ID3D12PipelineState> clearUintPso;

        SharedPtr<Shader> histogramShader;
        ComPtr<ID3D12RootSignature> histogramRootSig;
        ComPtr<ID3D12PipelineState> histogramPso;

        Texture exposureTexture;
        SharedPtr<Shader> exposureShader;
        ComPtr<ID3D12RootSignature> exposureRootSig;
        ComPtr<ID3D12PipelineState> exposurePso;

        Texture adaptedExposureTexture;
        Texture finalExposureTexture;
        SharedPtr<Shader> adaptExposureShader;
        ComPtr<ID3D12RootSignature> adaptExposureRootSig;
        ComPtr<ID3D12PipelineState> adaptExposurePso;
    } m_Resources{};

    bool m_ResetHistory = true;
};
