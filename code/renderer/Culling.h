// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Primitive.h"
#include "Raytracing.h"
#include "RenderSceneTypes.h"

struct CpuTimers;

using PrimitiveBucketRow = Array<Vector<PrimitiveRenderData>, CullMode_Count>;
using PrimitiveBuckets = Array<PrimitiveBucketRow, AlphaMode_Count>;

struct BuildParams
{
    const Vector<Primitive>* primitives = nullptr;
    const Vector<Material>* materials = nullptr;
    const Vector<InstanceData>* instances = nullptr;
    const XMFLOAT4X4* view = nullptr;
    f32 nearPlane = 0.0f;
    f32 frustumCullFovDeg = 0.0f;
    f32 aspectRatio = 0.0f;
    bool cpuFrustumCullingEnabled = false;
    bool updateInstances = false;
    bool debugMeshletColorEnabled = false;
    u32 materialsBufferSrvIndex = 0u;
    CpuTimers* cpuTimers = nullptr;
};

class Culling
{
  public:
    void Reset();
    void Build(const BuildParams& params);
    const PrimitiveBuckets& GetPrimitiveBuckets() const;
    const Vector<Raytracing::RTInstance>& GetRTInstances() const;

  private:
    PrimitiveBuckets m_PrimitiveBuckets{};
    Vector<XMFLOAT4X4> m_PrevInstanceWorlds;
    Vector<Raytracing::RTInstance> m_RTInstances;
    u32 m_RasterSubmittedCount = 0;
    u32 m_RasterCulledCount = 0;
};
