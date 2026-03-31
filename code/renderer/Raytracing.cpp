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
constexpr u32 kCacheCSGroupSize = 256u;
constexpr u32 kClearCacheDispatchX = 1024u;
constexpr f32 kPruneStartOccupancy = 0.80f;
constexpr f32 kPruneStopOccupancy = 0.65f;
constexpr f32 kPruneForceOccupancy = 0.95f;
constexpr u32 kPruneBudgetEntriesPerFrame = 1u << 15;
constexpr u32 kPruneMinAgeFrames = 512u;
constexpr u32 kPruneMinSamplesToKeep = 32u;
constexpr u32 kPruneAttemptsPerThread = 4u;
constexpr u32 kRadianceCacheMaxSamplesSafeClamp = 262144u;

constexpr u32 kShadowPayloadBytes = sizeof(u32);
constexpr u32 kPathTracePayloadBytes = 52u;
constexpr u32 kTriangleAttribBytes = 2u * sizeof(f32);

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
    m_Cleared = false;
    m_PruneActive = false;
    CreateShadowPassResources(renderSize);
    CreatePathTracePassResources(renderSize);
    CreateSpecularPassResources(renderSize);
}

void Raytracing::ReloadShaders()
{
    CreateShadowPassPipelines();
    CreatePathTracePassPipelines();
    CreateSpecularPassPipelines();
}

void Raytracing::InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<LoadedPrimitive>& loadedPrimitives, const Vector<RTInstance>& instances)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
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

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.Transform3x4 = 0;
    geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = 1;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    Vector<RTPrimInfo> primInfos(primitives.size());

    for (u32 primIndex = 0; primIndex < primitives.size(); ++primIndex)
    {
        Primitive& prim = primitives[primIndex];
        const LoadedPrimitive& srcPrim = loadedPrimitives[primIndex];
        const u32 indexCount = srcPrim.indexCount;
        const u32 vertexCount = srcPrim.vertexCount;

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

        geom.Flags = (primAlphaMode[primIndex] == static_cast<u32>(AlphaMode_Opaque)) ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

        geom.Triangles.IndexCount = indexCount;
        geom.Triangles.VertexCount = vertexCount;
        geom.Triangles.IndexBuffer = prim.rtIndices->resource->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StartAddress = prim.rtVertices->resource->GetGPUVirtualAddress();

        in.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

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

void Raytracing::CreateShadowPassResources(const XMUINT2& renderSize)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    const CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const CD3DX12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(
        device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Shadow.trace.outputTexture.resource)));
    m_Shadow.trace.outputTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_Shadow.trace.outputTexture.SetName(L"RT Shadows Output");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc{};
    outputUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Shadow.trace.outputTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_Shadow.trace.outputTexture.resource, outputUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outputSrvDesc{};
    outputSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    outputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outputSrvDesc.Texture2D.MipLevels = 1;
    m_Shadow.trace.outputTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_Shadow.trace.outputTexture.resource, outputSrvDesc);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_Shadow.blur.intermediateResource.resource)));
    m_Shadow.blur.intermediateResource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_Shadow.blur.intermediateResource.SetName(L"RTShadows_BlurIntermediate");

    D3D12_UNORDERED_ACCESS_VIEW_DESC intermediateUavDesc{};
    intermediateUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    intermediateUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Shadow.blur.intermediateResource.uavIndex = m_BindlessHeaps.CreateUAV(m_Shadow.blur.intermediateResource.resource, intermediateUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC intermediateSrvDesc = outputSrvDesc;
    m_Shadow.blur.intermediateResource.srvIndex = m_BindlessHeaps.CreateSRV(m_Shadow.blur.intermediateResource.resource, intermediateSrvDesc);
}

void Raytracing::CreatePathTracePassResources(const XMUINT2& renderSize)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();
    const XMUINT2 halfRes = {IE_DivRoundUp(renderSize.x, 2u), IE_DivRoundUp(renderSize.y, 2u)};

    const CD3DX12_RESOURCE_DESC traceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, halfRes.x, halfRes.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const CD3DX12_RESOURCE_DESC outDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const CD3DX12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &traceDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_PathTrace.trace.indirectDiffuseTraceTexture.resource)));
    m_PathTrace.trace.indirectDiffuseTraceTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_PathTrace.trace.indirectDiffuseTraceTexture.SetName(L"Indirect Diffuse Trace Half");

    D3D12_UNORDERED_ACCESS_VIEW_DESC traceUavDesc{};
    traceUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    traceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_PathTrace.trace.indirectDiffuseTraceTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_PathTrace.trace.indirectDiffuseTraceTexture.resource, traceUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC traceSrvDesc{};
    traceSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    traceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    traceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    traceSrvDesc.Texture2D.MipLevels = 1;
    m_PathTrace.trace.indirectDiffuseTraceTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_PathTrace.trace.indirectDiffuseTraceTexture.resource, traceSrvDesc);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_PathTrace.trace.indirectDiffuseTexture.resource)));
    m_PathTrace.trace.indirectDiffuseTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_PathTrace.trace.indirectDiffuseTexture.SetName(L"Indirect Diffuse");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
    outUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_PathTrace.trace.indirectDiffuseTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_PathTrace.trace.indirectDiffuseTexture.resource, outUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
    outSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outSrvDesc.Texture2D.MipLevels = 1;
    m_PathTrace.trace.indirectDiffuseTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_PathTrace.trace.indirectDiffuseTexture.resource, outSrvDesc);

    BufferCreateDesc d{};
    d.sizeInBytes = RC_ENTRIES * sizeof(RadianceCacheEntry);
    d.heapType = D3D12_HEAP_TYPE_DEFAULT;
    d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    d.finalState = d.initialState;
    d.viewKind = BufferCreateDesc::ViewKind::Structured;
    d.strideInBytes = sizeof(RadianceCacheEntry);
    d.createSRV = true;
    d.createUAV = true;
    d.name = L"RadianceCache";
    m_PathTrace.trace.radianceCache = CreateBuffer(nullptr, d);

    d.sizeInBytes = halfRes.x * halfRes.y * sizeof(RadianceSample);
    d.strideInBytes = sizeof(RadianceSample);
    d.name = L"RadianceSamples";
    m_PathTrace.trace.radianceSamples = CreateBuffer(nullptr, d);

    d.sizeInBytes = sizeof(u32);
    d.strideInBytes = sizeof(u32);
    d.name = L"RadianceCacheUsageCounter";
    m_PathTrace.trace.radianceCacheUsageCounter = CreateBuffer(nullptr, d);

    BufferCreateDesc rb{};
    rb.sizeInBytes = sizeof(u32);
    rb.heapType = D3D12_HEAP_TYPE_READBACK;
    rb.viewKind = BufferCreateDesc::ViewKind::None;
    rb.createSRV = false;
    rb.createUAV = false;
    rb.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    rb.finalState = D3D12_RESOURCE_STATE_COPY_DEST;
    rb.name = L"RadianceCacheUsageReadback";

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        m_PathTrace.trace.radianceCacheUsageReadback[i] = CreateBuffer(nullptr, rb);

        u32* mapped = nullptr;
        const D3D12_RANGE readRange(0, 0);
        IE_Check(m_PathTrace.trace.radianceCacheUsageReadback[i]->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
        *mapped = 0u;
        const D3D12_RANGE writeRange(0, 0);
        m_PathTrace.trace.radianceCacheUsageReadback[i]->Unmap(0, &writeRange);
    }
}

void Raytracing::CreateSpecularPassResources(const XMUINT2& renderSize)
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    const CD3DX12_RESOURCE_DESC outDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const CD3DX12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(
        device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Specular.trace.outputTexture.resource)));
    m_Specular.trace.outputTexture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_Specular.trace.outputTexture.SetName(L"RT Specular");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
    outUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Specular.trace.outputTexture.uavIndex = m_BindlessHeaps.CreateUAV(m_Specular.trace.outputTexture.resource, outUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
    outSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outSrvDesc.Texture2D.MipLevels = 1;
    m_Specular.trace.outputTexture.srvIndex = m_BindlessHeaps.CreateSRV(m_Specular.trace.outputTexture.resource, outSrvDesc);
}

void Raytracing::CreateShadowPassPipelines()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    Shader::ReloadOrCreate(m_Shadow.trace.shader, IE_SHADER_TYPE_LIB, "systems/raytracing/shadows/shadows.rt.hlsl", {});
    CreateGlobalRootSigConstants(device, sizeof(RTShadowsTraceConstants) / sizeof(u32), m_Shadow.trace.rootSig);

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    auto* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    const D3D12_SHADER_BYTECODE libdxil = m_Shadow.trace.shader->GetBytecode();
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
    shaderConfig->Config(kShadowPayloadBytes, kTriangleAttribBytes);

    auto* globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_Shadow.trace.rootSig.Get());

    auto* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(1);

    IE_Check(device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_Shadow.trace.dxrStateObject)));

    ComPtr<ID3D12StateObjectProperties> props;
    IE_Check(m_Shadow.trace.dxrStateObject.As(&props));

    const u32 shaderRecordSize = CalcShaderRecordSize();
    CreateShaderTable(device, props, L"RayGenShaderTable", L"Raygen", shaderRecordSize, m_Shadow.trace.rayGenShaderTable);
    CreateShaderTable(device, props, L"MissShaderTable", L"Miss", shaderRecordSize, m_Shadow.trace.missShaderTable);
    CreateShaderTable(device, props, L"HitGroupShaderTable", L"HitGroup", shaderRecordSize, m_Shadow.trace.hitGroupShaderTable);

    Shader::ReloadOrCreate(m_Shadow.blur.cs, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/shadows/shadows_blur.cs.hlsl", {});

    m_Shadow.blur.rootSignature = m_Shadow.blur.cs->GetOrCreateRootSignature(device);

    D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoDesc{};
    blurPsoDesc.pRootSignature = m_Shadow.blur.rootSignature.Get();
    blurPsoDesc.CS = m_Shadow.blur.cs->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&blurPsoDesc, IID_PPV_ARGS(&m_Shadow.blur.pso)));
}

void Raytracing::CreatePathTracePassPipelines()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    Shader::ReloadOrCreate(m_PathTrace.trace.shader, IE_SHADER_TYPE_LIB, "systems/raytracing/pathtrace/pathtrace.rt.hlsl", {});
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

    auto* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(1);

    IE_Check(device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_PathTrace.trace.dxrStateObject)));

    ComPtr<ID3D12StateObjectProperties> props;
    IE_Check(m_PathTrace.trace.dxrStateObject.As(&props));

    const u32 shaderRecordSize = CalcShaderRecordSize();
    CreateShaderTable(device, props, L"RayGenShaderTable", L"Raygen", shaderRecordSize, m_PathTrace.trace.rayGenShaderTable);
    CreateShaderTable(device, props, L"MissShaderTable", L"Miss", shaderRecordSize, m_PathTrace.trace.missShaderTable);
    CreateShaderTable(device, props, L"HitGroupShaderTable", L"HitGroup", shaderRecordSize, m_PathTrace.trace.hitGroupShaderTable);

    Shader::ReloadOrCreate(m_PathTrace.cache.csClearSamples, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/pathtrace/pathtrace_clear_samples.cs.hlsl", {});
    Shader::ReloadOrCreate(m_PathTrace.cache.csIntegrateSamples, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/pathtrace/pathtrace_integrate_samples.cs.hlsl", {});
    Shader::ReloadOrCreate(m_PathTrace.cache.csClearCache, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/pathtrace/pathtrace_clear_cache.cs.hlsl", {});
    Shader::ReloadOrCreate(m_PathTrace.cache.csPruneCache, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/pathtrace/pathtrace_prune_cache.cs.hlsl", {});
    Shader::ReloadOrCreate(m_PathTrace.upsample.cs, IE_SHADER_TYPE_COMPUTE, "systems/raytracing/pathtrace/pathtrace_upsample.cs.hlsl", {});

    m_PathTrace.cache.rootSignature = m_PathTrace.cache.csClearSamples->GetOrCreateRootSignature(device);
    m_PathTrace.upsample.rootSignature = m_PathTrace.upsample.cs->GetOrCreateRootSignature(device);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cachePsoDesc{};
    cachePsoDesc.pRootSignature = m_PathTrace.cache.rootSignature.Get();
    cachePsoDesc.CS = m_PathTrace.cache.csClearSamples->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&cachePsoDesc, IID_PPV_ARGS(&m_PathTrace.cache.clearPso)));
    cachePsoDesc.CS = m_PathTrace.cache.csIntegrateSamples->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&cachePsoDesc, IID_PPV_ARGS(&m_PathTrace.cache.integratePso)));
    cachePsoDesc.CS = m_PathTrace.cache.csClearCache->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&cachePsoDesc, IID_PPV_ARGS(&m_PathTrace.cache.clearCachePso)));
    cachePsoDesc.CS = m_PathTrace.cache.csPruneCache->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&cachePsoDesc, IID_PPV_ARGS(&m_PathTrace.cache.pruneCachePso)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC upsamplePsoDesc{};
    upsamplePsoDesc.pRootSignature = m_PathTrace.upsample.rootSignature.Get();
    upsamplePsoDesc.CS = m_PathTrace.upsample.cs->GetBytecode();
    IE_Check(device->CreateComputePipelineState(&upsamplePsoDesc, IID_PPV_ARGS(&m_PathTrace.upsample.pso)));
}

void Raytracing::CreateSpecularPassPipelines()
{
    const ComPtr<ID3D12Device14>& device = m_RenderDevice.GetDevice();

    Shader::ReloadOrCreate(m_Specular.trace.shader, IE_SHADER_TYPE_LIB, "systems/raytracing/specular/specular.rt.hlsl", {});
    CreateGlobalRootSigConstants(device, sizeof(RTSpecularConstants) / sizeof(u32), m_Specular.trace.rootSig);

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    auto* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    const D3D12_SHADER_BYTECODE libdxil = m_Specular.trace.shader->GetBytecode();
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
    globalRootSignature->SetRootSignature(m_Specular.trace.rootSig.Get());

    auto* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(1);

    IE_Check(device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_Specular.trace.dxrStateObject)));

    ComPtr<ID3D12StateObjectProperties> props;
    IE_Check(m_Specular.trace.dxrStateObject.As(&props));

    const u32 shaderRecordSize = CalcShaderRecordSize();
    CreateShaderTable(device, props, L"Specular/RayGenShaderTable", L"Raygen", shaderRecordSize, m_Specular.trace.rayGenShaderTable);
    CreateShaderTable(device, props, L"Specular/MissShaderTable", L"Miss", shaderRecordSize, m_Specular.trace.missShaderTable);
    CreateShaderTable(device, props, L"Specular/HitGroupShaderTable", L"HitGroup", shaderRecordSize, m_Specular.trace.hitGroupShaderTable);
}

void Raytracing::ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const ShadowPassInput& input)
{
    Camera& camera = m_Camera;

    const Camera::FrameData cameraFrameData = camera.GetFrameData();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    static constexpr XMUINT2 ditherFactors[4] = {
        XMUINT2(1, 1),
        XMUINT2(1, 2),
        XMUINT2(2, 2),
        XMUINT2(4, 4),
    };
    static constexpr u32 rtTileCount[4] = {
        1,
        2,
        4,
        16,
    };

    const u32 shadowTypeIndex = static_cast<u32>(g_Settings.rtShadowsResolution);
    IE_Assert(shadowTypeIndex < 4);

    const XMUINT2 currentDitherFactors = ditherFactors[shadowTypeIndex];
    const u32 tileCount = rtTileCount[shadowTypeIndex];
    const u32 slot = input.frameIndex % tileCount;

    u32 idx = 0;
    if (tileCount < 4)
    {
        idx = slot;
    }
    else if (tileCount == 4)
    {
        static constexpr u32 invBayer2[4] = {0, 3, 1, 2};
        idx = invBayer2[slot];
    }
    else
    {
        static constexpr u32 invBayer4[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
        idx = invBayer4[slot];
    }

    const u32 shift = currentDitherFactors.x >> 1;
    const u32 mask = currentDitherFactors.x - 1;
    const XMUINT2 ditherOffset = XMUINT2(idx & mask, idx >> shift);

    D3D12_DISPATCH_RAYS_DESC dispatchRaysDesc{};
    dispatchRaysDesc.RayGenerationShaderRecord.StartAddress = m_Shadow.trace.rayGenShaderTable->GetGPUVirtualAddress();
    dispatchRaysDesc.RayGenerationShaderRecord.SizeInBytes = m_Shadow.trace.rayGenShaderTable->GetDesc().Width;

    dispatchRaysDesc.MissShaderTable.StartAddress = m_Shadow.trace.missShaderTable->GetGPUVirtualAddress();
    dispatchRaysDesc.MissShaderTable.SizeInBytes = m_Shadow.trace.missShaderTable->GetDesc().Width;
    dispatchRaysDesc.MissShaderTable.StrideInBytes = m_Shadow.trace.missShaderTable->GetDesc().Width;

    dispatchRaysDesc.HitGroupTable.StartAddress = m_Shadow.trace.hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchRaysDesc.HitGroupTable.SizeInBytes = m_Shadow.trace.hitGroupShaderTable->GetDesc().Width;
    dispatchRaysDesc.HitGroupTable.StrideInBytes = m_Shadow.trace.hitGroupShaderTable->GetDesc().Width;

    dispatchRaysDesc.Depth = 1;
    dispatchRaysDesc.Width = IE_DivRoundUp(renderSize.x, currentDitherFactors.x);
    dispatchRaysDesc.Height = IE_DivRoundUp(renderSize.y, currentDitherFactors.y);

    RTShadowsTraceConstants constants{};
    constants.invViewProj = cameraFrameData.invViewProj;
    constants.outputTextureIndex = m_Shadow.trace.outputTexture.uavIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(dispatchRaysDesc.Width * currentDitherFactors.x), 1.0f / static_cast<f32>(dispatchRaysDesc.Height * currentDitherFactors.y));
    constants.ditherFactors = currentDitherFactors;
    constants.ditherOffset = ditherOffset;
    constants.sunDir = input.sunDir;
    constants.cameraPos = cameraFrameData.position;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.normalGeoTextureIndex = input.normalGeoTextureIndex;
    constants.primInfoBufferIndex = m_RTPrimInfoBuffer->srvIndex;
    constants.materialsBufferIndex = input.materialsBufferIndex;
    constants.tlasIndex = m_TlasSrvIndex;

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Ray-Traced Shadows");
    {
        m_Shadow.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_Shadow.trace.rootSig.Get());
        cmd->SetPipelineState1(m_Shadow.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsTraceConstants) / sizeof(u32), &constants, 0);
        cmd->DispatchRays(&dispatchRaysDesc);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Ray-Traced Shadows Blur");
    {
        RTShadowsBlurConstants rootConstants{};
        rootConstants.zNear = cameraFrameData.znearfar.x;
        rootConstants.depthTextureIndex = input.depthTextureIndex;
        rootConstants.normalGeoTextureIndex = input.normalGeoTextureIndex;

        const u32 dispatchX = IE_DivRoundUp(renderSize.x, 16);
        const u32 dispatchY = IE_DivRoundUp(renderSize.y, 16);

        cmd->SetComputeRootSignature(m_Shadow.blur.rootSignature.Get());
        cmd->SetPipelineState(m_Shadow.blur.pso.Get());

        m_Shadow.trace.outputTexture.UavBarrier(cmd);
        m_Shadow.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_Shadow.blur.intermediateResource.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        rootConstants.inputTextureIndex = m_Shadow.trace.outputTexture.srvIndex;
        rootConstants.outputTextureIndex = m_Shadow.blur.intermediateResource.uavIndex;
        rootConstants.axis = {1u, 0u};
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        m_Shadow.blur.intermediateResource.UavBarrier(cmd);
        m_Shadow.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_Shadow.blur.intermediateResource.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        rootConstants.inputTextureIndex = m_Shadow.blur.intermediateResource.srvIndex;
        rootConstants.outputTextureIndex = m_Shadow.trace.outputTexture.uavIndex;
        rootConstants.axis = {0u, 1u};
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        m_Shadow.trace.outputTexture.UavBarrier(cmd);
        m_Shadow.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

void Raytracing::PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const PathTracePassInput& input)
{
    Camera& camera = m_Camera;
    const XMUINT2 halfRes = {IE_DivRoundUp(renderSize.x, 2u), IE_DivRoundUp(renderSize.y, 2u)};

    const Camera::FrameData cameraFrameData = camera.GetFrameData();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();

    bool cacheWasCleared = false;
    if (!m_Cleared)
    {
        ClearPathTraceRadianceCacheCS(cmd, gpuTimers);
        m_Cleared = true;
        cacheWasCleared = true;
    }
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    const u32 samplesCount = halfRes.x * halfRes.y;
    const u32 readbackIdx = input.frameInFlightIdx % IE_Constants::frameInFlightCount;

    u32 usedEntriesPrev = 0u;
    const SharedPtr<Buffer>& usageReadback = m_PathTrace.trace.radianceCacheUsageReadback[readbackIdx];
    if (usageReadback && usageReadback->resource)
    {
        const D3D12_RANGE readRange(0, sizeof(u32));
        u32* mapped = nullptr;
        if (SUCCEEDED(usageReadback->Map(0, &readRange, reinterpret_cast<void**>(&mapped))) && mapped)
        {
            usedEntriesPrev = *mapped;
            const D3D12_RANGE writeRange(0, 0);
            usageReadback->Unmap(0, &writeRange);
        }
    }

    const f32 occupancy = static_cast<f32>(usedEntriesPrev) / static_cast<f32>(RC_ENTRIES);
    if (cacheWasCleared)
    {
        m_PruneActive = false;
    }
    else if (!m_PruneActive && occupancy >= kPruneStartOccupancy)
    {
        m_PruneActive = true;
    }
    else if (m_PruneActive && occupancy <= kPruneStopOccupancy)
    {
        m_PruneActive = false;
    }

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Radiance Cache - Clear Samples");
    {
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.clearPso.Get());

        m_PathTrace.trace.radianceSamples->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheClearSamplesConstants c{};
        c.radianceSamplesUavIndex = m_PathTrace.trace.radianceSamples->uavIndex;
        c.samplesCount = samplesCount;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(samplesCount, kCacheCSGroupSize), 1, 1);

        m_PathTrace.trace.radianceSamples->UavBarrier(cmd);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    D3D12_DISPATCH_RAYS_DESC dispatchRayDesc{};
    dispatchRayDesc.RayGenerationShaderRecord.StartAddress = m_PathTrace.trace.rayGenShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.RayGenerationShaderRecord.SizeInBytes = m_PathTrace.trace.rayGenShaderTable->GetDesc().Width;

    dispatchRayDesc.MissShaderTable.StartAddress = m_PathTrace.trace.missShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.MissShaderTable.SizeInBytes = m_PathTrace.trace.missShaderTable->GetDesc().Width;
    dispatchRayDesc.MissShaderTable.StrideInBytes = m_PathTrace.trace.missShaderTable->GetDesc().Width;

    dispatchRayDesc.HitGroupTable.StartAddress = m_PathTrace.trace.hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.HitGroupTable.SizeInBytes = m_PathTrace.trace.hitGroupShaderTable->GetDesc().Width;
    dispatchRayDesc.HitGroupTable.StrideInBytes = m_PathTrace.trace.hitGroupShaderTable->GetDesc().Width;

    dispatchRayDesc.Width = halfRes.x;
    dispatchRayDesc.Height = halfRes.y;
    dispatchRayDesc.Depth = 1;

    PathTraceConstants constants{};
    constants.invViewProj = cameraFrameData.invViewProj;
    constants.cameraPos = cameraFrameData.position;
    constants.indirectDiffuseTextureIndex = m_PathTrace.trace.indirectDiffuseTraceTexture.uavIndex;
    constants.sunDir = input.sunDir;
    constants.normalGeoTextureIndex = input.normalGeoTextureIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(renderSize.x), 1.0f / static_cast<f32>(renderSize.y));
    constants.tlasIndex = m_TlasSrvIndex;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.primInfoBufferIndex = m_RTPrimInfoBuffer->srvIndex;
    constants.materialsBufferIndex = input.materialsBufferIndex;
    constants.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
    constants.radianceCacheSrvIndex = m_PathTrace.trace.radianceCache->srvIndex;
    constants.radianceCacheCellSize = g_Settings.radianceCacheCellSize;
    constants.frameIndex = input.frameIndex;
    constants.radianceSamplesUavIndex = m_PathTrace.trace.radianceSamples->uavIndex;
    constants.samplesCount = samplesCount;
    constants.skyCubeIndex = input.skyCubeIndex;
    constants.skyIntensity = g_Settings.skyIntensity;
    constants.samplerIndex = input.samplerIndex;
    const XMVECTOR sunDir = XMLoadFloat3(&input.sunDir);
    const XMVECTOR sunDirNormalized = XMVector3Normalize(sunDir);
    XMStoreFloat3(&constants.sunDir, sunDirNormalized);
    const XMVECTOR sunColor = XMLoadFloat3(&input.sunColor);
    const XMVECTOR sunColorClamped = XMVectorMax(sunColor, XMVectorZero());
    XMFLOAT3 sunColorClampedF3{};
    XMStoreFloat3(&sunColorClampedF3, sunColorClamped);
    const f32 luminance = sunColorClampedF3.x * 0.2126f + sunColorClampedF3.y * 0.7152f + sunColorClampedF3.z * 0.0722f;
    XMFLOAT3 sunRadiance = {};
    if (luminance > 1e-4f)
    {
        const f32 invLuminance = 1.0f / luminance;
        sunRadiance.x = sunColorClampedF3.x * invLuminance * g_Settings.sunIntensity;
        sunRadiance.y = sunColorClampedF3.y * invLuminance * g_Settings.sunIntensity;
        sunRadiance.z = sunColorClampedF3.z * invLuminance * g_Settings.sunIntensity;
    }
    constants.sunRadiance = sunRadiance;
    constants.spp = g_Settings.pathTraceSpp;
    constants.bounceCount = g_Settings.pathTraceBounceCount;
    constants.useTrilinear = g_Settings.radianceCacheTrilinear ? 1u : 0u;
    constants.useSoftNormalInterpolation = g_Settings.radianceCacheSoftNormalInterpolation ? 1u : 0u;
    constants.softNormalMinDot = g_Settings.radianceCacheSoftNormalMinDot;
    constants.trilinearMinCornerSamples = g_Settings.radianceCacheTrilinearMinCornerSamples;
    constants.trilinearMinHits = g_Settings.radianceCacheTrilinearMinHits;
    constants.trilinearPresentMinSamples = g_Settings.radianceCacheTrilinearPresentMinSamples;
    constants.normalBinRes = g_Settings.radianceCacheNormalBinRes;
    constants.minExtraSppCount = g_Settings.radianceCacheMinExtraSppCount;
    constants.maxProbes = g_Settings.radianceCacheMaxProbes;
    const u32 clampedMaxSamples = (g_Settings.radianceCacheMaxSamples < kRadianceCacheMaxSamplesSafeClamp) ? g_Settings.radianceCacheMaxSamples : kRadianceCacheMaxSamplesSafeClamp;
    constants.maxSamples = clampedMaxSamples;

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Path Tracing");
    {
        m_PathTrace.trace.indirectDiffuseTraceTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_PathTrace.trace.rootSig.Get());
        cmd->SetPipelineState1(m_PathTrace.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(PathTraceConstants) / sizeof(u32), &constants, 0);

        cmd->DispatchRays(&dispatchRayDesc);

        m_PathTrace.trace.radianceSamples->UavBarrier(cmd);
        m_PathTrace.trace.indirectDiffuseTraceTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Path Tracing - Upsample");
    {
        m_PathTrace.trace.indirectDiffuseTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceUpsampleConstants c{};
        c.inputTextureIndex = m_PathTrace.trace.indirectDiffuseTraceTexture.srvIndex;
        c.outputTextureIndex = m_PathTrace.trace.indirectDiffuseTexture.uavIndex;
        c.depthTextureIndex = input.depthTextureIndex;
        c.normalGeoTextureIndex = input.normalGeoTextureIndex;

        cmd->SetComputeRootSignature(m_PathTrace.upsample.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.upsample.pso.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(renderSize.x, 8u), IE_DivRoundUp(renderSize.y, 8u), 1);

        m_PathTrace.trace.indirectDiffuseTexture.UavBarrier(cmd);
        m_PathTrace.trace.indirectDiffuseTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    if (m_PruneActive)
    {
        GPU_MARKER_BEGIN(cmd, gpuTimers, "Radiance Cache - Prune");
        {
            cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
            cmd->SetPipelineState(m_PathTrace.cache.pruneCachePso.Get());

            m_PathTrace.trace.radianceCache->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_PathTrace.trace.radianceCacheUsageCounter->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const bool forcePrune = occupancy >= kPruneForceOccupancy;

            PathTraceCachePruneConstants c{};
            c.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
            c.radianceCacheUsageCounterUavIndex = m_PathTrace.trace.radianceCacheUsageCounter->uavIndex;
            c.frameIndex = input.frameIndex;
            c.minAgeToPrune = forcePrune ? 0u : kPruneMinAgeFrames;
            c.minSamplesToKeep = forcePrune ? 0u : kPruneMinSamplesToKeep;
            c.attemptsPerThread = kPruneAttemptsPerThread;
            c.randomSeed = input.frameIndex * 747796405u + 2891336453u;

            cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
            cmd->Dispatch(IE_DivRoundUp(kPruneBudgetEntriesPerFrame, kCacheCSGroupSize), 1, 1);

            m_PathTrace.trace.radianceCache->UavBarrier(cmd);
            m_PathTrace.trace.radianceCacheUsageCounter->UavBarrier(cmd);
        }
        GPU_MARKER_END(cmd, gpuTimers);
    }

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Radiance Cache - Integrate Samples");
    {
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.integratePso.Get());

        m_PathTrace.trace.radianceSamples->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_PathTrace.trace.radianceCache->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_PathTrace.trace.radianceCacheUsageCounter->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheIntegrateSamplesConstants c;
        c.radianceSamplesSrvIndex = m_PathTrace.trace.radianceSamples->srvIndex;
        c.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
        c.radianceCacheUsageCounterUavIndex = m_PathTrace.trace.radianceCacheUsageCounter->uavIndex;
        c.samplesCount = samplesCount;
        c.frameIndex = input.frameIndex;
        c.maxProbes = g_Settings.radianceCacheMaxProbes;
        c.maxSamples = clampedMaxSamples;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(samplesCount, kCacheCSGroupSize), 1, 1);

        m_PathTrace.trace.radianceCache->UavBarrier(cmd);
        m_PathTrace.trace.radianceCacheUsageCounter->UavBarrier(cmd);

        m_PathTrace.trace.radianceCacheUsageCounter->Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(m_PathTrace.trace.radianceCacheUsageReadback[readbackIdx]->Get(), 0, m_PathTrace.trace.radianceCacheUsageCounter->Get(), 0, sizeof(u32));
        m_PathTrace.trace.radianceCacheUsageCounter->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_PathTrace.trace.radianceCache->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_PathTrace.trace.radianceSamples->Transition(cmd, D3D12_RESOURCE_STATE_COMMON);
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

void Raytracing::SpecularPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers, const XMUINT2& renderSize, const SpecularPassInput& input)
{
    Camera& camera = m_Camera;

    const Camera::FrameData cameraFrameData = camera.GetFrameData();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();
    cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

    static constexpr XMUINT2 ditherFactors[4] = {
        XMUINT2(1, 1),
        XMUINT2(1, 2),
        XMUINT2(2, 2),
        XMUINT2(4, 4),
    };
    static constexpr u32 rtTileCount[4] = {
        1,
        2,
        4,
        16,
    };
    const u32 specularTypeIndex = static_cast<u32>(g_Settings.rtSpecularResolution);
    IE_Assert(specularTypeIndex < 4);
    const XMUINT2 currentDitherFactors = ditherFactors[specularTypeIndex];
    const u32 tileCount = rtTileCount[specularTypeIndex];
    const u32 slot = input.frameIndex % tileCount;

    u32 idx = 0;
    if (tileCount < 4)
    {
        idx = slot;
    }
    else if (tileCount == 4)
    {
        static constexpr u32 invBayer2[4] = {0, 3, 1, 2};
        idx = invBayer2[slot];
    }
    else
    {
        static constexpr u32 invBayer4[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
        idx = invBayer4[slot];
    }

    const u32 shift = currentDitherFactors.x >> 1;
    const u32 mask = currentDitherFactors.x - 1;
    const XMUINT2 ditherOffset = XMUINT2(idx & mask, idx >> shift);

    D3D12_DISPATCH_RAYS_DESC dispatchRayDesc{};
    dispatchRayDesc.RayGenerationShaderRecord.StartAddress = m_Specular.trace.rayGenShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.RayGenerationShaderRecord.SizeInBytes = m_Specular.trace.rayGenShaderTable->GetDesc().Width;

    dispatchRayDesc.MissShaderTable.StartAddress = m_Specular.trace.missShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.MissShaderTable.SizeInBytes = m_Specular.trace.missShaderTable->GetDesc().Width;
    dispatchRayDesc.MissShaderTable.StrideInBytes = m_Specular.trace.missShaderTable->GetDesc().Width;

    dispatchRayDesc.HitGroupTable.StartAddress = m_Specular.trace.hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchRayDesc.HitGroupTable.SizeInBytes = m_Specular.trace.hitGroupShaderTable->GetDesc().Width;
    dispatchRayDesc.HitGroupTable.StrideInBytes = m_Specular.trace.hitGroupShaderTable->GetDesc().Width;

    dispatchRayDesc.Width = IE_DivRoundUp(renderSize.x, currentDitherFactors.x);
    dispatchRayDesc.Height = IE_DivRoundUp(renderSize.y, currentDitherFactors.y);
    dispatchRayDesc.Depth = 1;

    RTSpecularConstants constants{};
    constants.invViewProj = cameraFrameData.invViewProj;
    constants.cameraPos = cameraFrameData.position;
    constants.outputTextureIndex = m_Specular.trace.outputTexture.uavIndex;
    constants.sunDir = input.sunDir;
    constants.normalTextureIndex = input.normalTextureIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(renderSize.x), 1.0f / static_cast<f32>(renderSize.y));
    constants.tlasIndex = m_TlasSrvIndex;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.materialTextureIndex = input.materialTextureIndex;
    constants.primInfoBufferIndex = m_RTPrimInfoBuffer->srvIndex;
    constants.materialsBufferIndex = input.materialsBufferIndex;
    constants.skyCubeIndex = input.skyCubeIndex;
    constants.samplerIndex = input.samplerIndex;
    constants.skyIntensity = g_Settings.skyIntensity;
    constants.sunIntensity = g_Settings.sunIntensity;
    constants.frameIndex = input.frameIndex;
    constants.sunColor = input.sunColor;
    constants.roughnessRaySpread = 0.8f;
    constants.sppMin = g_Settings.rtSpecularSppMin;
    constants.sppMax = g_Settings.rtSpecularSppMax;
    constants.ditherFactors = currentDitherFactors;
    constants.ditherOffset = ditherOffset;

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Ray-Traced Specular");
    {
        m_Specular.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_Specular.trace.rootSig.Get());
        cmd->SetPipelineState1(m_Specular.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTSpecularConstants) / sizeof(u32), &constants, 0);

        cmd->DispatchRays(&dispatchRayDesc);

        m_Specular.trace.outputTexture.UavBarrier(cmd);
        m_Specular.trace.outputTexture.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);
}

const Raytracing::PathTracePassResources& Raytracing::GetPathTracePassResources() const
{
    return m_PathTrace;
}

const Raytracing::SpecularPassResources& Raytracing::GetSpecularPassResources() const
{
    return m_Specular;
}

const Raytracing::ShadowPassResources& Raytracing::GetShadowPassResources() const
{
    return m_Shadow;
}

void Raytracing::ClearPathTraceRadianceCacheCS(const ComPtr<ID3D12GraphicsCommandList7>& cmd, GpuTimers& gpuTimers)
{
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = m_BindlessHeaps.GetDescriptorHeaps();

    GPU_MARKER_BEGIN(cmd, gpuTimers, "Radiance Cache - Clear Cache");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.clearCachePso.Get());

        m_PathTrace.trace.radianceCache->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheClearCacheConstants c;
        c.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
        c.cacheEntries = RC_ENTRIES;
        const u32 groupCount = IE_DivRoundUp(RC_ENTRIES, kCacheCSGroupSize);
        const u32 dispatchX = kClearCacheDispatchX;
        const u32 dispatchY = IE_DivRoundUp(groupCount, dispatchX);
        c.dispatchGroupsX = dispatchX;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        m_PathTrace.trace.radianceCache->UavBarrier(cmd);
        const u32 zero = 0u;
        SetBufferData(cmd, m_PathTrace.trace.radianceCacheUsageCounter, &zero, sizeof(zero), 0);
        m_PathTrace.trace.radianceCache->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, gpuTimers);

    m_PruneActive = false;
}

void Raytracing::InvalidatePathTraceRadianceCache()
{
    m_Cleared = false;
    m_PruneActive = false;
}
