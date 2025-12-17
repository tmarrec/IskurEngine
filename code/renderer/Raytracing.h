// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Primitive.h"
#include "Shader.h"

class Raytracing : public Singleton<Raytracing>
{
  public:
    struct RTInstance
    {
        u32 primIndex;
        u32 materialIndex;
        XMFLOAT4X4 world;
    };

    void Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances);
    void ReloadShaders();

    void InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances);

    void CreateShadowPassResources();
    void CreatePathTracePassResources();
    void ClearPathTraceRadianceCacheCS(const ComPtr<ID3D12GraphicsCommandList7>& cmd);

    void CreateShadowPassPipelines();
    void CreatePathTracePassPipelines();
    struct ShadowPassInput
    {
        u32 depthTextureIndex;
        XMFLOAT3 sunDir;
        u32 frameIndex;
    };
    void ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ShadowPassInput& input);

    struct ShadowPassResources
    {
        struct Trace
        {
            SharedPtr<Shader> shader;
            ComPtr<ID3D12Resource> missShaderTable;
            ComPtr<ID3D12Resource> hitGroupShaderTable;
            ComPtr<ID3D12Resource> rayGenShaderTable;
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12StateObject> dxrStateObject;
            ComPtr<ID3D12Resource> outputTexture;
            u32 outputUavIndex = UINT_MAX;
            u32 outputSrvIndex = UINT_MAX;
        };

        struct Blur
        {
            ComPtr<ID3D12Resource> intermediateResource;
            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> horizontalPso;
            ComPtr<ID3D12PipelineState> verticalPso;

            u32 srvRawIdx = UINT32_MAX;
            u32 srvIntermediateIdx = UINT32_MAX;
            u32 uavIntermediateIdx = UINT32_MAX;

            SharedPtr<Shader> csH;
            SharedPtr<Shader> csV;
        };

        Trace trace;
        Blur blur;
    };
    const ShadowPassResources& GetShadowPassResources() const;

    struct PathTracePassInput
    {
        u32 depthTextureIndex;
        XMFLOAT3 sunDir;
        u32 frameIndex;
        u32 normalGeoTextureIndex;
        u32 albedoTextureIndex;
        u32 materialsBufferIndex;
        u32 samplerIndex;
    };
    void PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const PathTracePassInput& input);

    struct PathTracePassResources
    {
        struct Trace
        {
            SharedPtr<Shader> shader;
            ComPtr<ID3D12Resource> missShaderTable;
            ComPtr<ID3D12Resource> hitGroupShaderTable;
            ComPtr<ID3D12Resource> rayGenShaderTable;
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12StateObject> dxrStateObject;
            ComPtr<ID3D12Resource> indirectDiffuseTexture;
            u32 outputUavIndex = UINT_MAX;
            u32 outputSrvIndex = UINT_MAX;

            SharedPtr<Buffer> radianceCache;
            SharedPtr<Buffer> radianceSamples;
        };

        struct CacheCS
        {
            SharedPtr<Shader> csClearSamples;
            SharedPtr<Shader> csIntegrateSamples;
            SharedPtr<Shader> csClearCache;

            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> clearPso;
            ComPtr<ID3D12PipelineState> integratePso;
            ComPtr<ID3D12PipelineState> clearCachePso;
        };

        Trace trace;
        CacheCS cache;
    };
    const PathTracePassResources& GetPathTracePassResources() const;

    SharedPtr<Buffer> m_RTPrimInfoBuffer;

  private:
    SharedPtr<Buffer> m_TlasScratch;
    SharedPtr<Buffer> m_Tlas;
    SharedPtr<Buffer> m_InstanceDescs;

    u32 m_TlasSrvIndex = UINT_MAX;

    ShadowPassResources m_Shadow;
    PathTracePassResources m_PathTrace;

    bool m_Cleared = false;
};
