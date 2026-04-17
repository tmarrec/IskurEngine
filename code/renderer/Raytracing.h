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

    void CreatePathTracePassResources(const XMUINT2& renderSize);
    void CreatePathTracePassPipelines();
    void InvalidatePathTraceDescriptorIndices();

    struct PathTracePassInput
    {
        u32 depthTextureIndex;
        u32 albedoTextureIndex;
        u32 normalTextureIndex;
        u32 normalGeoTextureIndex;
        u32 materialTextureIndex;
        u32 emissiveTextureIndex;
        XMFLOAT3 sunDir;
        XMFLOAT3 sunColor;
        f32 sunDiskAngleDeg;
        u32 frameIndex;
        u32 materialsBufferIndex;
        u32 skyCubeIndex;
        u32 samplerIndex;
    };
    void PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const PathTracePassInput& input);

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
            Texture outputTexture;
            Texture hitDistanceTexture;
        };

        Trace trace;
    };
    PathTracePassResources& GetPathTracePassResources();
    const PathTracePassResources& GetPathTracePassResources() const;

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

    PathTracePassResources m_PathTrace;
};

