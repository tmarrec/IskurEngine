// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Constants.h"
#include "Primitive.h"
#include "Shader.h"
#include "Timings.h"
#include "Texture.h"

class BindlessHeaps;
class Camera;
class RenderDevice;
struct LoadedPrimitive;

class Raytracing
{
  public:
    Raytracing(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps, Camera& camera);

    Raytracing(const Raytracing&) = delete;
    Raytracing& operator=(const Raytracing&) = delete;

    struct RTInstance
    {
        u32 primIndex;
        u32 materialIndex;
        u32 alphaMode;
        XMFLOAT4X4 world;
    };

    void Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, const XMUINT2& renderSize, Vector<Primitive>& primitives, const Vector<LoadedPrimitive>& loadedPrimitives,
              const Vector<RTInstance>& instances);
    void ReloadShaders();

    void InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<LoadedPrimitive>& loadedPrimitives,
                             const Vector<RTInstance>& instances);
    void UpdateInstances(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Vector<RTInstance>& instances);

    void CreateShadowPassResources(const XMUINT2& renderSize);
    void CreatePathTracePassResources(const XMUINT2& renderSize);
    void CreateSpecularPassResources(const XMUINT2& renderSize);
    void ClearPathTraceRadianceCacheCS(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers);
    void InvalidatePathTraceRadianceCache();

    void CreateShadowPassPipelines();
    void CreatePathTracePassPipelines();
    void CreateSpecularPassPipelines();
    struct ShadowPassInput
    {
        u32 depthTextureIndex;
        u32 normalGeoTextureIndex;
        u32 materialsBufferIndex;
        XMFLOAT3 sunDir;
        u32 frameIndex;
    };
    void ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const ShadowPassInput& input);

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
            Texture outputTexture;
        };

        struct Blur
        {
            Texture intermediateResource;
            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> pso;

            SharedPtr<Shader> cs;
        };

        Trace trace;
        Blur blur;
    };
    const ShadowPassResources& GetShadowPassResources() const;

    struct PathTracePassInput
    {
        u32 depthTextureIndex;
        XMFLOAT3 sunDir;
        XMFLOAT3 sunColor;
        u32 frameIndex;
        u32 frameInFlightIdx;
        u32 normalGeoTextureIndex;
        u32 materialsBufferIndex;
        u32 skyCubeIndex;
        u32 samplerIndex;
    };
    void PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const PathTracePassInput& input);

    struct SpecularPassInput
    {
        u32 depthTextureIndex;
        u32 normalTextureIndex;
        u32 materialTextureIndex;
        XMFLOAT3 sunDir;
        XMFLOAT3 sunColor;
        u32 frameIndex;
        u32 materialsBufferIndex;
        u32 skyCubeIndex;
        u32 samplerIndex;
    };
    void SpecularPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const SpecularPassInput& input);

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
            Texture indirectDiffuseTraceTexture;
            Texture indirectDiffuseTexture;

            SharedPtr<Buffer> radianceCache;
            SharedPtr<Buffer> radianceSamples;
            SharedPtr<Buffer> radianceCacheUsageCounter;
            Array<SharedPtr<Buffer>, IE_Constants::frameInFlightCount> radianceCacheUsageReadback = {};
        };

        struct Upsample
        {
            SharedPtr<Shader> cs;
            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> pso;
        };

        struct CacheCS
        {
            SharedPtr<Shader> csClearSamples;
            SharedPtr<Shader> csIntegrateSamples;
            SharedPtr<Shader> csClearCache;
            SharedPtr<Shader> csPruneCache;

            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> clearPso;
            ComPtr<ID3D12PipelineState> integratePso;
            ComPtr<ID3D12PipelineState> clearCachePso;
            ComPtr<ID3D12PipelineState> pruneCachePso;
        };

        Trace trace;
        Upsample upsample;
        CacheCS cache;
    };
    const PathTracePassResources& GetPathTracePassResources() const;

    struct SpecularPassResources
    {
        struct Trace
        {
            SharedPtr<Shader> shader;
            ComPtr<ID3D12Resource> missShaderTable;
            ComPtr<ID3D12Resource> hitGroupShaderTable;
            ComPtr<ID3D12Resource> rayGenShaderTable;
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12StateObject> dxrStateObject;
            Texture outputTexture;
        };

        Trace trace;
    };
    const SpecularPassResources& GetSpecularPassResources() const;

    SharedPtr<Buffer> m_RTPrimInfoBuffer;

  private:
    SharedPtr<Buffer> CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc);
    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    RenderDevice& m_RenderDevice;
    BindlessHeaps& m_BindlessHeaps;
    Camera& m_Camera;

    SharedPtr<Buffer> m_TlasScratch;
    SharedPtr<Buffer> m_Tlas;
    SharedPtr<Buffer> m_InstanceDescs;
    Vector<Primitive>* m_Primitives = nullptr;

    u32 m_TlasSrvIndex = UINT_MAX;
    u32 m_InstanceCount = 0;

    ShadowPassResources m_Shadow;
    PathTracePassResources m_PathTrace;
    SpecularPassResources m_Specular;

    bool m_Cleared = false;
    bool m_PruneActive = false;
};

