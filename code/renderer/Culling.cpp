// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Culling.h"

#include "RuntimeState.h"
#include "Timings.h"

namespace
{
f32 ComputeMaxWorldScale(const XMFLOAT4X4& world)
{
    const f32 rowX = IE_Sqrt(world._11 * world._11 + world._12 * world._12 + world._13 * world._13);
    const f32 rowY = IE_Sqrt(world._21 * world._21 + world._22 * world._22 + world._23 * world._23);
    const f32 rowZ = IE_Sqrt(world._31 * world._31 + world._32 * world._32 + world._33 * world._33);
    return IE_Max(rowX, IE_Max(rowY, rowZ));
}

f32 ComputeWorldSign(const XMFLOAT4X4& world)
{
    const f32 det =
        world._11 * (world._22 * world._33 - world._23 * world._32) - world._12 * (world._21 * world._33 - world._23 * world._31) + world._13 * (world._21 * world._32 - world._22 * world._31);
    return det < 0.0f ? -1.0f : 1.0f;
}

bool IsSphereVisibleViewFrustum(const XMFLOAT3& viewCenter, const f32 radius, const f32 nearPlane, const f32 tanHalfX, const f32 tanHalfY)
{
    if (radius <= 0.0f)
    {
        return true;
    }

    // RH view space: camera looks down -Z, near plane at z = -nearPlane.
    if (viewCenter.z + nearPlane > radius)
    {
        return false;
    }

    const f32 sideXLen = IE_Sqrt(1.0f + tanHalfX * tanHalfX);
    const f32 sideYLen = IE_Sqrt(1.0f + tanHalfY * tanHalfY);
    const f32 sideXRadius = radius * sideXLen;
    const f32 sideYRadius = radius * sideYLen;

    // RH frustum side planes through origin:
    //  right:  x + z*tanHalfX <= 0
    //  left:  -x + z*tanHalfX <= 0
    //  top:    y + z*tanHalfY <= 0
    //  bottom:-y + z*tanHalfY <= 0
    if (viewCenter.x + viewCenter.z * tanHalfX > sideXRadius)
    {
        return false;
    }
    if (-viewCenter.x + viewCenter.z * tanHalfX > sideXRadius)
    {
        return false;
    }
    if (viewCenter.y + viewCenter.z * tanHalfY > sideYRadius)
    {
        return false;
    }
    if (-viewCenter.y + viewCenter.z * tanHalfY > sideYRadius)
    {
        return false;
    }

    return true;
}

bool IsPrimitiveInFrustum(const Primitive& prim, const XMFLOAT4X4& world, const f32 maxWorldScale, const XMFLOAT4X4& view, const f32 nearPlane, const f32 tanHalfX, const f32 tanHalfY)
{
    if (prim.localBoundsRadius <= 0.0f)
    {
        return true;
    }

    XMFLOAT3 worldCenterRow{};
    {
        const XMMATRIX worldM = XMLoadFloat4x4(&world);
        const XMVECTOR localCenter = XMLoadFloat3(&prim.localBoundsCenter);
        XMStoreFloat3(&worldCenterRow, XMVector3TransformCoord(localCenter, worldM));
    }

    const f32 worldRadius = prim.localBoundsRadius * maxWorldScale;
    if (worldRadius <= 0.0f)
    {
        return true;
    }

    XMFLOAT3 viewCenter{};
    {
        const XMMATRIX viewM = XMLoadFloat4x4(&view);
        const XMVECTOR worldCenter = XMLoadFloat3(&worldCenterRow);
        XMStoreFloat3(&viewCenter, XMVector3TransformCoord(worldCenter, viewM));
    }
    return IsSphereVisibleViewFrustum(viewCenter, worldRadius, nearPlane, tanHalfX, tanHalfY);
}
} // namespace

void Culling::Reset()
{
    for (u32 am = 0; am < AlphaMode_Count; ++am)
    {
        for (u32 cm = 0; cm < CullMode_Count; ++cm)
        {
            m_PrimitiveBuckets[am][cm].clear();
        }
    }
    m_RTInstances.clear();
    m_PrevInstanceWorlds.clear();
    m_RasterSubmittedCount = 0;
    m_RasterCulledCount = 0;
    g_Stats.cpuFrustumCullTotalInstances = 0;
    g_Stats.cpuFrustumCullRasterSubmitted = 0;
    g_Stats.cpuFrustumCullRasterCulled = 0;
}

void Culling::Build(const BuildParams& params)
{
    CPU_MARKER_BEGIN(*params.cpuTimers, "Culling and Draw List Build");

    const Vector<Primitive>& primitives = *params.primitives;
    const Vector<Material>& materials = *params.materials;
    const Vector<InstanceData>& instances = *params.instances;
    const XMFLOAT4X4& view = *params.view;

    // Reset per-frame outputs.
    for (u32 am = 0; am < AlphaMode_Count; ++am)
    {
        for (u32 cm = 0; cm < CullMode_Count; ++cm)
        {
            m_PrimitiveBuckets[am][cm].clear();
        }
    }

    m_RTInstances.clear();
    if (params.updateInstances)
    {
        m_RTInstances.reserve(instances.size());
    }

    // Bootstrap motion history for new/changed instance counts.
    if (m_PrevInstanceWorlds.size() != instances.size())
    {
        m_PrevInstanceWorlds.resize(instances.size());
        for (u32 i = 0; i < instances.size(); ++i)
        {
            m_PrevInstanceWorlds[i] = instances[i].world;
        }
    }

    m_RasterSubmittedCount = 0;
    m_RasterCulledCount = 0;

    const f32 tanHalfY = std::tan(IE_ToRadians(params.frustumCullFovDeg) * 0.5f);
    const f32 tanHalfX = tanHalfY * params.aspectRatio;

    IE_Assert(m_PrevInstanceWorlds.size() == instances.size());
    IE_Assert(params.nearPlane > 0.0f);
    IE_Assert(params.aspectRatio > 0.0f);
    IE_Assert(params.frustumCullFovDeg > 0.0f && params.frustumCullFovDeg < 180.0f);

    for (u32 i = 0; i < instances.size(); ++i)
    {
        const InstanceData& inst = instances[i];
        const XMFLOAT4X4& prevWorld = m_PrevInstanceWorlds[i];

        IE_Assert(inst.primIndex < primitives.size());
        IE_Assert(inst.materialIndex < materials.size());

        const Material& mat = materials[inst.materialIndex];
        IE_Assert(mat.alphaMode < static_cast<u32>(AlphaMode_Count));
        const AlphaMode am = static_cast<AlphaMode>(mat.alphaMode);
        const CullMode cm = mat.doubleSided ? CullMode_None : CullMode_Back;

        const Primitive& prim = primitives[inst.primIndex];
        const f32 maxWorldScale = ComputeMaxWorldScale(inst.world);
        const bool rasterVisible = !params.cpuFrustumCullingEnabled || IsPrimitiveInFrustum(prim, inst.world, maxWorldScale, view, params.nearPlane, tanHalfX, tanHalfY);

        if (rasterVisible)
        {
            XMMATRIX Mworld = XMLoadFloat4x4(&inst.world);
            XMMATRIX MworldInv = XMMatrixInverse(nullptr, Mworld);
            XMFLOAT4X4 worldInv{};
            XMStoreFloat4x4(&worldInv, MworldInv);

            PrimitiveConstants pc{};
            pc.world = inst.world;
            pc.prevWorld = prevWorld;
            pc.worldInv = worldInv;
            pc.meshletCount = prim.meshletCount;
            pc.materialIdx = inst.materialIndex;
            pc.verticesBufferIndex = prim.vertices->srvIndex;
            pc.meshletsBufferIndex = prim.meshlets->srvIndex;
            pc.meshletVerticesBufferIndex = prim.mlVerts->srvIndex;
            pc.meshletTrianglesBufferIndex = prim.mlTris->srvIndex;
            pc.meshletBoundsBufferIndex = prim.mlBounds->srvIndex;
            pc.materialsBufferIndex = params.materialsBufferSrvIndex;
            pc.debugMeshletColorEnabled = params.debugMeshletColorEnabled ? 1u : 0u;
            pc.maxWorldScale = maxWorldScale;
            pc.worldSign = ComputeWorldSign(inst.world);
            pc.allowBackfaceConeCull = mat.doubleSided ? 0u : 1u;

            PrimitiveRenderData prd{};
            prd.primIndex = inst.primIndex;
            prd.primConstants = pc;
            m_PrimitiveBuckets[am][cm].push_back(prd);
            m_RasterSubmittedCount++;
        }
        else
        {
            m_RasterCulledCount++;
        }

        if (params.updateInstances)
        {
            Raytracing::RTInstance rti{};
            rti.primIndex = inst.primIndex;
            rti.materialIndex = inst.materialIndex;
            rti.alphaMode = mat.alphaMode;
            rti.world = inst.world;
            m_RTInstances.push_back(rti);
        }

        m_PrevInstanceWorlds[i] = inst.world;
    }

    g_Stats.cpuFrustumCullTotalInstances = static_cast<u32>(instances.size());
    g_Stats.cpuFrustumCullRasterSubmitted = m_RasterSubmittedCount;
    g_Stats.cpuFrustumCullRasterCulled = m_RasterCulledCount;

    CPU_MARKER_END(*params.cpuTimers);
}

const PrimitiveBuckets& Culling::GetPrimitiveBuckets() const
{
    return m_PrimitiveBuckets;
}

const Vector<Raytracing::RTInstance>& Culling::GetRTInstances() const
{
    return m_RTInstances;
}
