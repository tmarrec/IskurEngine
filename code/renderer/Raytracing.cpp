// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Raytracing.h"

#include "BindlessHeaps.h"
#include "Camera.h"
#include "RenderDevice.h"
#include "RuntimeState.h"
#include "SceneLoader.h"
#include "Shader.h"

namespace
{
constexpr u32 kInvalidMaterialIdx = 0xFFFFFFFFu;

constexpr u32 kPathTracePayloadBytes = 72u;
constexpr u32 kTriangleAttribBytes = 2u * sizeof(f32);

void RequireOpacityMicromapSupport(const ComPtr<ID3D12Device14>& device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    IE_Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_2)
    {
        IE_LogFatal("Opacity micromaps are mandatory but the device only supports DXR tier {}.", static_cast<u32>(options5.RaytracingTier));
        IE_Assert(false);
    }
}

void RequireShaderExecutionReorderingSupport(const ComPtr<ID3D12Device14>& device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22{};
    IE_Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &options22, sizeof(options22)));
    if (!options22.ShaderExecutionReorderingActuallyReorders)
    {
        IE_LogFatal("Shader execution reordering is mandatory but the device does not support it.");
        IE_Assert(false);
    }
}

D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT ToD3D12OpacityMicromapFormat(const u32 format)
{
    switch (format)
    {
    case 2:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE;
    case 4:
        return D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
    default:
        IE_LogFatal("Unsupported serialized OMM format {}.", format);
        IE_Assert(false);
        return D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
    }
}

Vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> BuildOpacityMicromapHistogram(const LoadedPrimitive& prim)
{
    constexpr u32 kMaxSubdivLevel = D3D12_RAYTRACING_OPACITY_MICROMAP_OC1_MAX_SUBDIVISION_LEVEL;
    Vector<u32> counts(kMaxSubdivLevel + 1u, 0u);
    for (u32 i = 0; i < prim.ommDescCount; ++i)
    {
        IE_Assert(prim.ommDescs[i].subdivisionLevel <= kMaxSubdivLevel);
        counts[prim.ommDescs[i].subdivisionLevel] += 1u;
    }

    Vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
    for (u32 level = 0; level <= kMaxSubdivLevel; ++level)
    {
        if (counts[level] == 0u)
            continue;

        D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY entry{};
        entry.Count = counts[level];
        entry.SubdivisionLevel = level;
        entry.Format = ToD3D12OpacityMicromapFormat(prim.ommFormat);
        histogram.push_back(entry);
    }
    return histogram;
}

void FillInstanceTransform(D3D12_RAYTRACING_INSTANCE_DESC& dst, const XMFLOAT4X4& m)
{
    dst.Transform[0][0] = m._11;
    dst.Transform[0][1] = m._21;
    dst.Transform[0][2] = m._31;
    dst.Transform[0][3] = m._41;

    dst.Transform[1][0] = m._12;
    dst.Transform[1][1] = m._22;
    dst.Transform[1][2] = m._32;
    dst.Transform[1][3] = m._42;

    dst.Transform[2][0] = m._13;
    dst.Transform[2][1] = m._23;
    dst.Transform[2][2] = m._33;
    dst.Transform[2][3] = m._43;
}

u32 CalcShaderRecordSize()
{
    constexpr u32 recordAligned = IE_AlignUp(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    return IE_AlignUp(recordAligned, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
}

void ReleaseTextureDescriptors(BindlessHeaps& bindlessHeaps, Texture& texture)
{
    bindlessHeaps.FreeCbvSrvUav(texture.srvIndex);
    bindlessHeaps.FreeCbvSrvUav(texture.uavIndex);
    texture.srvIndex = UINT32_MAX;
    texture.uavIndex = UINT32_MAX;
}

void CreateGlobalRootSigConstants(const ComPtr<ID3D12Device14>& device, u32 num32BitConstants, ComPtr<ID3D12RootSignature>& outRootSig)
{
    CD3DX12_ROOT_PARAMETER rootParameter{};
    rootParameter.InitAsConstants(num32BitConstants, 0);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Init(1, &rootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC versioned(rsDesc);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    IE_Check(D3D12SerializeVersionedRootSignature(&versioned, &blob, &error));

    IE_Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&outRootSig)));
}

void CreateShaderTable(const ComPtr<ID3D12Device14>& device, const ComPtr<ID3D12StateObjectProperties>& props, const wchar_t* tableName, const wchar_t* exportName, u32 recordSize,
                       ComPtr<ID3D12Resource>& outTable)
{
    recordSize = IE_AlignUp(recordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(recordSize);
    const CD3DX12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    IE_Check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outTable)));

    IE_Check(outTable->SetName(tableName));

    u8* mappedData = nullptr;
    IE_Check(outTable->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)));

    std::memset(mappedData, 0, recordSize);
    std::memcpy(mappedData, props->GetShaderIdentifier(exportName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

    outTable->Unmap(0, nullptr);
}
} // namespace

Raytracing::Raytracing(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps, Camera& camera) : m_RenderDevice(renderDevice), m_BindlessHeaps(bindlessHeaps), m_Camera(camera)
{
}

SharedPtr<Buffer> Raytracing::CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc)
{
    return m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd, createDesc);
}

void Raytracing::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    m_RenderDevice.SetBufferData(cmd, dst, data, sizeInBytes, offsetInBytes);
}

void Raytracing::Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, const XMUINT2& renderSize, Vector<Primitive>& primitives, const Vector<LoadedPrimitive>& loadedPrimitives,
                      const Vector<RTInstance>& instances)
{
    InitRaytracingWorld(cmd, primitives, loadedPrimitives, instances);
    CreatePathTracePassResources(renderSize);
}

void Raytracing::ReloadShaders()
{
    CreatePathTracePassPipelines();
}

void Raytracing::InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<LoadedPrimitive>& loadedPrimitives, const Vector<RTInstance>& instances)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    RequireOpacityMicromapSupport(device);
    m_Primitives = &primitives;
    IE_Assert(primitives.size() == loadedPrimitives.size());

    Vector<u32> primMaterialIdx(primitives.size(), kInvalidMaterialIdx);
    Vector<u32> primAlphaMode(primitives.size(), static_cast<u32>(AlphaMode_Opaque));
    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());

        u32& dst = primMaterialIdx[inst.primIndex];
        if (dst == kInvalidMaterialIdx)
        {
            dst = inst.materialIndex;
            primAlphaMode[inst.primIndex] = inst.alphaMode;
        }
        else
        {
            IE_Assert(dst == inst.materialIndex);
            IE_Assert(primAlphaMode[inst.primIndex] == inst.alphaMode);
        }
    }

    for (u32& m : primMaterialIdx)
    {
        if (m == kInvalidMaterialIdx)
        {
            m = 0;
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
    in.NumDescs = 1;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    Vector<RTPrimInfo> primInfos(primitives.size());

    for (u32 primIndex = 0; primIndex < primitives.size(); ++primIndex)
    {
        Primitive& prim = primitives[primIndex];
        const LoadedPrimitive& srcPrim = loadedPrimitives[primIndex];
        const u32 indexCount = srcPrim.indexCount;
        const u32 vertexCount = srcPrim.vertexCount;
        const bool alphaTested = primAlphaMode[primIndex] != static_cast<u32>(AlphaMode_Opaque);

        BufferCreateDesc d{};
        d.heapType = D3D12_HEAP_TYPE_DEFAULT;
        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.createSRV = true;
        d.createUAV = false;
        d.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        d.finalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Reuse the raster vertex buffer for RT to avoid duplicating GPU uploads.
        prim.rtVertices = prim.vertices;
        IE_Assert(prim.rtVertices && prim.rtVertices->resource);
        prim.rtVertices->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        d.sizeInBytes = indexCount * sizeof(u32);
        d.initialDataSize = d.sizeInBytes;
        d.strideInBytes = sizeof(u32);
        d.initialData = srcPrim.indices;
        d.name = L"Primitive/rtIndices";
        prim.rtIndices = CreateBuffer(cmd.Get(), d);

        primInfos[primIndex].vbSrvIndex = prim.rtVertices->srvIndex;
        primInfos[primIndex].ibSrvIndex = prim.rtIndices->srvIndex;
        primInfos[primIndex].materialIdx = primMaterialIdx[primIndex];

        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC trianglesDesc{};
        trianglesDesc.Transform3x4 = 0;
        trianglesDesc.IndexFormat = DXGI_FORMAT_R32_UINT;
        trianglesDesc.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        trianglesDesc.IndexCount = indexCount;
        trianglesDesc.VertexCount = vertexCount;
        trianglesDesc.IndexBuffer = prim.rtIndices->resource->GetGPUVirtualAddress();
        trianglesDesc.VertexBuffer.StartAddress = prim.rtVertices->resource->GetGPUVirtualAddress();
        trianglesDesc.VertexBuffer.StrideInBytes = sizeof(Vertex);

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Flags = alphaTested ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC ommLinkage{};

        if (alphaTested)
        {
            if (srcPrim.ommFormat == 0u || srcPrim.ommIndexCount != (indexCount / 3u) || srcPrim.ommIndices == nullptr)
            {
                IE_LogFatal("Alpha-tested primitive {} is missing required opacity micromap index data.", primIndex);
                IE_Assert(false);
            }

            d.viewKind = BufferCreateDesc::ViewKind::None;
            d.createSRV = false;
            d.createUAV = false;
            d.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            d.finalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            d.resourceFlags = D3D12_RESOURCE_FLAG_NONE;
            d.initialData = srcPrim.ommIndices;
            d.initialDataSize = srcPrim.ommIndexCount * sizeof(i32);
            d.sizeInBytes = d.initialDataSize;
            d.name = L"Primitive/ommIndices";
            prim.ommIndices = CreateBuffer(cmd.Get(), d);

            if (srcPrim.ommDescCount > 0u)
            {
                IE_Assert(srcPrim.ommDescs != nullptr);
                IE_Assert(srcPrim.ommData != nullptr);
                IE_Assert(srcPrim.ommDataByteCount > 0u);

                Vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> ommDescs(srcPrim.ommDescCount);
                for (u32 i = 0; i < srcPrim.ommDescCount; ++i)
                {
                    const IEPack::OpacityMicromapDescRecord& srcDesc = srcPrim.ommDescs[i];
                    D3D12_RAYTRACING_OPACITY_MICROMAP_DESC& dstDesc = ommDescs[i];
                    dstDesc.ByteOffset = srcDesc.dataByteOffset;
                    dstDesc.SubdivisionLevel = static_cast<UINT16>(srcDesc.subdivisionLevel);
                    dstDesc.Format = ToD3D12OpacityMicromapFormat(srcPrim.ommFormat);
                }

                d.initialData = ommDescs.data();
                d.initialDataSize = static_cast<u32>(ommDescs.size() * sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC));
                d.sizeInBytes = d.initialDataSize;
                d.name = L"Primitive/ommDescs";
                prim.ommDescs = CreateBuffer(cmd.Get(), d);

                d.initialData = srcPrim.ommData;
                d.initialDataSize = srcPrim.ommDataByteCount;
                d.sizeInBytes = srcPrim.ommDataByteCount;
                d.name = L"Primitive/ommData";
                prim.ommData = CreateBuffer(cmd.Get(), d);

                const Vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram = BuildOpacityMicromapHistogram(srcPrim);
                IE_Assert(!histogram.empty());

                D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ommArrayDesc{};
                ommArrayDesc.NumOmmHistogramEntries = static_cast<UINT>(histogram.size());
                ommArrayDesc.pOmmHistogram = histogram.data();
                ommArrayDesc.InputBuffer = prim.ommData->resource->GetGPUVirtualAddress();
                ommArrayDesc.PerOmmDescs.StartAddress = prim.ommDescs->resource->GetGPUVirtualAddress();
                ommArrayDesc.PerOmmDescs.StrideInBytes = sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC);

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ommInputs{};
                ommInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
                ommInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
                ommInputs.NumDescs = 1u;
                ommInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                ommInputs.pOpacityMicromapArrayDesc = &ommArrayDesc;

                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ommInfo{};
                device->GetRaytracingAccelerationStructurePrebuildInfo(&ommInputs, &ommInfo);
                IE_Assert(ommInfo.ScratchDataSizeInBytes <= UINT32_MAX);
                IE_Assert(ommInfo.ResultDataMaxSizeInBytes <= UINT32_MAX);

                d.initialData = nullptr;
                d.initialDataSize = 0u;
                d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                d.finalState = d.initialState;
                d.sizeInBytes = static_cast<u32>(ommInfo.ScratchDataSizeInBytes);
                d.name = L"Primitive/ommArrayScratch";
                prim.ommArrayScratch = CreateBuffer(cmd.Get(), d);

                d.sizeInBytes = static_cast<u32>(ommInfo.ResultDataMaxSizeInBytes);
                d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
                d.finalState = d.initialState;
                d.name = L"Primitive/ommArray";
                prim.ommArray = CreateBuffer(nullptr, d);

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC ommBuild{};
                ommBuild.DestAccelerationStructureData = prim.ommArray->resource->GetGPUVirtualAddress();
                ommBuild.Inputs = ommInputs;
                ommBuild.ScratchAccelerationStructureData = prim.ommArrayScratch->resource->GetGPUVirtualAddress();
                cmd->BuildRaytracingAccelerationStructure(&ommBuild, 0, nullptr);

                const D3D12_RESOURCE_BARRIER ommBarrier = CD3DX12_RESOURCE_BARRIER::UAV(prim.ommArray->resource.Get());
                cmd->ResourceBarrier(1, &ommBarrier);
            }

            ommLinkage.OpacityMicromapIndexBuffer.StartAddress = prim.ommIndices->resource->GetGPUVirtualAddress();
            ommLinkage.OpacityMicromapIndexBuffer.StrideInBytes = sizeof(i32);
            ommLinkage.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
            ommLinkage.OpacityMicromapBaseLocation = 0u;
            ommLinkage.OpacityMicromapArray = prim.ommArray ? prim.ommArray->resource->GetGPUVirtualAddress() : 0ull;

            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
            geom.OmmTriangles.pTriangles = &trianglesDesc;
            geom.OmmTriangles.pOmmLinkage = &ommLinkage;
        }
        else
        {
            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom.Triangles = trianglesDesc;
        }

        in.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        IE_Assert(info.ScratchDataSizeInBytes <= UINT32_MAX);
        IE_Assert(info.ResultDataMaxSizeInBytes <= UINT32_MAX);

        d.initialData = nullptr;
        d.initialDataSize = 0;
        d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        d.finalState = d.initialState;
        d.viewKind = BufferCreateDesc::ViewKind::None;
        d.createSRV = false;
        d.createUAV = false;

        d.sizeInBytes = static_cast<u32>(info.ScratchDataSizeInBytes);
        d.name = L"BLAS Scratch";
        prim.blasScratch = CreateBuffer(cmd.Get(), d);

        d.sizeInBytes = static_cast<u32>(info.ResultDataMaxSizeInBytes);
        d.name = L"BLAS";
        d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        d.finalState = d.initialState;
        prim.blas = CreateBuffer(nullptr, d);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData = prim.blas->resource->GetGPUVirtualAddress();
        build.Inputs = in;
        build.ScratchAccelerationStructureData = prim.blasScratch->resource->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    }

    {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        cmd->ResourceBarrier(1, &uav);
    }

    {
        BufferCreateDesc d{};
        d.sizeInBytes = static_cast<u32>(primInfos.size() * sizeof(RTPrimInfo));
        d.strideInBytes = sizeof(RTPrimInfo);
        d.heapType = D3D12_HEAP_TYPE_DEFAULT;
        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.createSRV = true;
        d.createUAV = false;
        d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;
        d.name = L"RT PrimInfo Buffer";

        m_RTPrimInfoBuffer = CreateBuffer(nullptr, d);
        SetBufferData(cmd, m_RTPrimInfoBuffer, primInfos.data(), d.sizeInBytes, 0);
    }

    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.reserve(instances.size());

    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());
        const Primitive& prim = primitives[inst.primIndex];

        D3D12_RAYTRACING_INSTANCE_DESC idesc{};
        idesc.InstanceMask = 0xFF;
        idesc.Flags = g_Settings.rtOmmsEnabled ? D3D12_RAYTRACING_INSTANCE_FLAG_NONE : D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS;
        idesc.AccelerationStructure = prim.blas->resource->GetGPUVirtualAddress();
        idesc.InstanceID = inst.primIndex;

        FillInstanceTransform(idesc, inst.world);

        instanceDescs.push_back(idesc);
    }

    BufferCreateDesc d{};
    d.sizeInBytes = static_cast<u32>(instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    d.strideInBytes = 0;
    d.heapType = D3D12_HEAP_TYPE_UPLOAD;
    d.viewKind = BufferCreateDesc::ViewKind::None;
    d.createSRV = false;
    d.createUAV = false;
    d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.name = L"InstanceDescs";
    m_InstanceDescs = CreateBuffer(nullptr, d);

    {
        u8* mapped = nullptr;
        IE_Check(m_InstanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        std::memcpy(mapped, instanceDescs.data(), d.sizeInBytes);
        m_InstanceDescs->Unmap(0, nullptr);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.InstanceDescs = m_InstanceDescs->resource->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topInfo);

    d.heapType = D3D12_HEAP_TYPE_DEFAULT;
    d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    d.finalState = d.initialState;
    d.sizeInBytes = static_cast<u32>(topInfo.ScratchDataSizeInBytes);
    d.name = L"TLAS Scratch";
    m_TlasScratch = CreateBuffer(cmd.Get(), d);

    d.sizeInBytes = static_cast<u32>(topInfo.ResultDataMaxSizeInBytes);
    d.name = L"TLAS";
    d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    d.finalState = d.initialState;
    m_Tlas = CreateBuffer(nullptr, d);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
    topBuild.DestAccelerationStructureData = m_Tlas->resource->GetGPUVirtualAddress();
    topBuild.Inputs = topLevelInputs;
    topBuild.ScratchAccelerationStructureData = m_TlasScratch->resource->GetGPUVirtualAddress();

    cmd->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc{};
    tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tlasSrvDesc.RaytracingAccelerationStructure.Location = m_Tlas->resource->GetGPUVirtualAddress();
    m_TlasSrvIndex = m_BindlessHeaps.CreateSRV(nullptr, tlasSrvDesc);
    m_InstanceCount = static_cast<u32>(instanceDescs.size());
}

void Raytracing::UpdateInstances(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Vector<RTInstance>& instances)
{
    if (!cmd)
    {
        return;
    }
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    if (instances.empty())
    {
        return;
    }
    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.reserve(instances.size());

    if (!m_Primitives)
    {
        return;
    }
    const Vector<Primitive>& primitives = *m_Primitives;
    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());
        const Primitive& prim = primitives[inst.primIndex];

        D3D12_RAYTRACING_INSTANCE_DESC idesc{};
        idesc.InstanceMask = 0xFF;
        idesc.Flags = g_Settings.rtOmmsEnabled ? D3D12_RAYTRACING_INSTANCE_FLAG_NONE : D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS;
        idesc.AccelerationStructure = prim.blas->resource->GetGPUVirtualAddress();
        idesc.InstanceID = inst.primIndex;
        FillInstanceTransform(idesc, inst.world);

        instanceDescs.push_back(idesc);
    }

    const bool countChanged = (m_InstanceCount != static_cast<u32>(instanceDescs.size()));
    if (countChanged || !m_InstanceDescs || !m_Tlas || !m_TlasScratch)
    {
        BufferCreateDesc d{};
        d.sizeInBytes = static_cast<u32>(instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        d.strideInBytes = 0;
        d.heapType = D3D12_HEAP_TYPE_UPLOAD;
        d.viewKind = BufferCreateDesc::ViewKind::None;
        d.createSRV = false;
        d.createUAV = false;
        d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;
        d.name = L"InstanceDescs";
        m_InstanceDescs = CreateBuffer(nullptr, d);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
        topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
        topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        topLevelInputs.InstanceDescs = m_InstanceDescs->resource->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topInfo);

        d.heapType = D3D12_HEAP_TYPE_DEFAULT;
        d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        d.finalState = d.initialState;
        d.sizeInBytes = static_cast<u32>(topInfo.ScratchDataSizeInBytes);
        d.name = L"TLAS Scratch";
        m_TlasScratch = CreateBuffer(cmd.Get(), d);

        d.sizeInBytes = static_cast<u32>(topInfo.ResultDataMaxSizeInBytes);
        d.name = L"TLAS";
        d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        d.finalState = d.initialState;
        m_Tlas = CreateBuffer(nullptr, d);

        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc{};
        tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrvDesc.RaytracingAccelerationStructure.Location = m_Tlas->resource->GetGPUVirtualAddress();
        m_TlasSrvIndex = m_BindlessHeaps.CreateSRV(nullptr, tlasSrvDesc);

        m_InstanceCount = static_cast<u32>(instanceDescs.size());
    }

    {
        u8* mapped = nullptr;
        IE_Check(m_InstanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        std::memcpy(mapped, instanceDescs.data(), instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        m_InstanceDescs->Unmap(0, nullptr);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (!countChanged)
    {
        topLevelInputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }
    topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.InstanceDescs = m_InstanceDescs->resource->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
    topBuild.Inputs = topLevelInputs;
    topBuild.ScratchAccelerationStructureData = m_TlasScratch->resource->GetGPUVirtualAddress();
    topBuild.DestAccelerationStructureData = m_Tlas->resource->GetGPUVirtualAddress();
    if (!countChanged)
    {
        topBuild.SourceAccelerationStructureData = m_Tlas->resource->GetGPUVirtualAddress();
    }
    cmd->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);
}

void Raytracing::CreatePathTracePassResources(const XMUINT2& renderSize)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    ReleaseTextureDescriptors(m_BindlessHeaps, m_PathTrace.trace.outputTexture);
    ReleaseTextureDescriptors(m_BindlessHeaps, m_PathTrace.trace.hitDistanceTexture);

    const CD3DX12_RESOURCE_DESC outDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const CD3DX12_RESOURCE_DESC hitDistanceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_PathTrace.trace.outputTexture.resource)));
    m_PathTrace.trace.outputTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_PathTrace.trace.outputTexture.SetName(L"Path-Traced Lighting");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
    outUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_PathTrace.trace.outputTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_PathTrace.trace.outputTexture.resource, outUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
    outSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outSrvDesc.Texture2D.MipLevels = 1;
    m_PathTrace.trace.outputTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_PathTrace.trace.outputTexture.resource, outSrvDesc);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &hitDistanceDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_PathTrace.trace.hitDistanceTexture.resource)));
    m_PathTrace.trace.hitDistanceTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_PathTrace.trace.hitDistanceTexture.SetName(L"Path-Traced Lighting Specular Hit Distance");

    D3D12_UNORDERED_ACCESS_VIEW_DESC hitDistanceUavDesc{};
    hitDistanceUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    hitDistanceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_PathTrace.trace.hitDistanceTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_PathTrace.trace.hitDistanceTexture.resource, hitDistanceUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC hitDistanceSrvDesc{};
    hitDistanceSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    hitDistanceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hitDistanceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hitDistanceSrvDesc.Texture2D.MipLevels = 1;
    m_PathTrace.trace.hitDistanceTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_PathTrace.trace.hitDistanceTexture.resource, hitDistanceSrvDesc);
}

void Raytracing::InvalidatePathTraceDescriptorIndices()
{
    m_PathTrace.trace.outputTexture.srvIndex = UINT32_MAX;
    m_PathTrace.trace.outputTexture.uavIndex = UINT32_MAX;
    m_PathTrace.trace.hitDistanceTexture.srvIndex = UINT32_MAX;
    m_PathTrace.trace.hitDistanceTexture.uavIndex = UINT32_MAX;
}

void Raytracing::CreatePathTracePassPipelines()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    RequireOpacityMicromapSupport(device);
    RequireShaderExecutionReorderingSupport(device);

    Shader::ReloadOrCreate(m_PathTrace.trace.shader, IE_SHADER_TYPE_LIB, "systems/raytracing/lighting/path_trace.rt.hlsl", {});
    CreateGlobalRootSigConstants(device, sizeof(PathTraceConstants) / sizeof(u32), m_PathTrace.trace.rootSig);

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    auto* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    const D3D12_SHADER_BYTECODE libdxil = m_PathTrace.trace.shader->GetBytecode();
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"AnyHit");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"Miss");

    auto* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetAnyHitShaderImport(L"AnyHit");
    hitGroup->SetClosestHitShaderImport(L"ClosestHit");
    hitGroup->SetHitGroupExport(L"HitGroup");
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    auto* shaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shaderConfig->Config(kPathTracePayloadBytes, kTriangleAttribBytes);

    auto* globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_PathTrace.trace.rootSig.Get());

    auto* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG1_SUBOBJECT>();
    pipelineConfig->Config(1, D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS);

    IE_Check(device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_PathTrace.trace.dxrStateObject)));

    ComPtr<ID3D12StateObjectProperties> props;
    IE_Check(m_PathTrace.trace.dxrStateObject.As(&props));

    const u32 shaderRecordSize = CalcShaderRecordSize();
    CreateShaderTable(device, props, L"PathTrace/RayGenShaderTable", L"Raygen", shaderRecordSize, m_PathTrace.trace.rayGenShaderTable);
    CreateShaderTable(device, props, L"PathTrace/MissShaderTable", L"Miss", shaderRecordSize, m_PathTrace.trace.missShaderTable);
    CreateShaderTable(device, props, L"PathTrace/HitGroupShaderTable", L"HitGroup", shaderRecordSize, m_PathTrace.trace.hitGroupShaderTable);
}

void Raytracing::PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const PathTracePassInput& input)
{
    const Camera::FrameData cameraFrameData = m_Camera.GetFrameData();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    D3D12_DISPATCH_RAYS_DESC dispatchRayDesc{};
    dispatchRayDesc.RayGenerationShaderRecord.StartAddress = m_PathTrace.trace.rayGenShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.RayGenerationShaderRecord.SizeInBytes = m_PathTrace.trace.rayGenShaderTable->GetDesc().Width;
    dispatchRayDesc.MissShaderTable.StartAddress = m_PathTrace.trace.missShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.MissShaderTable.SizeInBytes = m_PathTrace.trace.missShaderTable->GetDesc().Width;
    dispatchRayDesc.MissShaderTable.StrideInBytes = m_PathTrace.trace.missShaderTable->GetDesc().Width;
    dispatchRayDesc.HitGroupTable.StartAddress = m_PathTrace.trace.hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.HitGroupTable.SizeInBytes = m_PathTrace.trace.hitGroupShaderTable->GetDesc().Width;
    dispatchRayDesc.HitGroupTable.StrideInBytes = m_PathTrace.trace.hitGroupShaderTable->GetDesc().Width;

    dispatchRayDesc.Width = renderSize.x;
    dispatchRayDesc.Height = renderSize.y;
    dispatchRayDesc.Depth = 1;

    PathTraceConstants constants{};
    constants.invViewProj = cameraFrameData.invViewProj;
    constants.cameraPos = cameraFrameData.position;
    constants.outputTextureIndex = m_PathTrace.trace.outputTexture.uavIndex;
    constants.hitDistanceTextureIndex = m_PathTrace.trace.hitDistanceTexture.uavIndex;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.albedoTextureIndex = input.albedoTextureIndex;
    constants.normalTextureIndex = input.normalTextureIndex;
    constants.normalGeoTextureIndex = input.normalGeoTextureIndex;
    constants.materialTextureIndex = input.materialTextureIndex;
    constants.emissiveTextureIndex = input.emissiveTextureIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(renderSize.x), 1.0f / static_cast<f32>(renderSize.y));
    constants.tlasIndex = m_TlasSrvIndex;
    constants.primInfoBufferIndex = m_RTPrimInfoBuffer->srvIndex;
    constants.materialsBufferIndex = input.materialsBufferIndex;
    constants.skyCubeIndex = input.skyCubeIndex;
    constants.samplerIndex = input.samplerIndex;
    constants.sunDir = input.sunDir;
    constants.frameIndex = input.frameIndex;
    constants.sunColor = input.sunColor;
    constants.rtPathTraceSpp = IE_Max(g_Settings.rtPathTraceSpp, 1u);
    constants.skyIntensity = g_Settings.skyIntensity;
    constants.sunIntensity = g_Settings.sunIntensity;
    constants.sunDiskAngleDeg = IE_Clamp(input.sunDiskAngleDeg, 0.001f, 12.0f);
    constants.shadowMinVisibility = g_Settings.shadowMinVisibility;
    constants.specularShadowMinVisibility = g_Settings.specularShadowMinVisibility;
    constants.rtMaxBounces = IE_Max(g_Settings.rtMaxBounces, 1u);
    constants.rtShadowsEnabled = g_Settings.rtShadowsEnabled ? 1u : 0u;
    constants.rtUse2StateOmmRays = g_Settings.rtUse2StateOmmRays ? 1u : 0u;
    constants.rtOmmsEnabled = g_Settings.rtOmmsEnabled ? 1u : 0u;
    constants.rtSerEnabled = g_Settings.rtSerEnabled ? 1u : 0u;
    constants.debugViewMode = g_Settings.lightingDebugMode;
    constants.debugMeshletColorEnabled = g_Settings.debugMeshletColor ? 1u : 0u;

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Path-Traced Lighting");
    {
        m_PathTrace.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_PathTrace.trace.hitDistanceTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_PathTrace.trace.rootSig.Get());
        cmd->SetPipelineState1(m_PathTrace.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(PathTraceConstants) / sizeof(u32), &constants, 0);

        cmd->DispatchRays(&dispatchRayDesc);

        m_PathTrace.trace.outputTexture.UavBarrier(cmd);
        m_PathTrace.trace.hitDistanceTexture.UavBarrier(cmd);
        m_PathTrace.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_PathTrace.trace.hitDistanceTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

Raytracing::PathTracePassResources& Raytracing::GetPathTracePassResources()
{
    return m_PathTrace;
}

const Raytracing::PathTracePassResources& Raytracing::GetPathTracePassResources() const
{
    return m_PathTrace;
}

