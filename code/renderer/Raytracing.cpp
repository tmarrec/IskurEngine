// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Raytracing.h"

#include "Camera.h"
#include "ImGui.h"
#include "LoadShader.h"
#include "Renderer.h"
#include "Shader.h"

namespace
{
constexpr u32 kInvalidMaterialIdx = 0xFFFFFFFFu;
constexpr u32 kCacheCSGroupSize = 256u;

constexpr u32 kShadowPayloadBytes = sizeof(u32);
constexpr u32 kPathTracePayloadBytes = 40u;
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

void Raytracing::Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances)
{
    InitRaytracingWorld(cmd, primitives, instances);

    CreateShadowPassResources();
    CreatePathTracePassResources();

    ReloadShaders();
}

void Raytracing::ReloadShaders()
{
    CreateShadowPassPipelines();
    CreatePathTracePassPipelines();
}

void Raytracing::InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances)
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();

    Vector<u32> primMaterialIdx(primitives.size(), kInvalidMaterialIdx);
    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());

        u32& dst = primMaterialIdx[inst.primIndex];
        if (dst == kInvalidMaterialIdx)
        {
            dst = inst.materialIndex;
        }
        else
        {
            IE_Assert(dst == inst.materialIndex);
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

        BufferCreateDesc d{};
        d.heapType = D3D12_HEAP_TYPE_DEFAULT;
        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.createSRV = true;
        d.createUAV = false;
        d.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        d.finalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        d.sizeInBytes = prim.vertexCount * sizeof(Vertex);
        d.initialDataSize = d.sizeInBytes;
        d.strideInBytes = sizeof(Vertex);
        d.initialData = prim.cpuVertices;
        d.name = L"Primitive/rtVertices";
        prim.rtVertices = renderer.CreateBuffer(cmd.Get(), d);

        d.sizeInBytes = prim.indexCount * sizeof(u32);
        d.initialDataSize = d.sizeInBytes;
        d.strideInBytes = sizeof(u32);
        d.initialData = prim.cpuIndices;
        d.name = L"Primitive/rtIndices";
        prim.rtIndices = renderer.CreateBuffer(cmd.Get(), d);

        primInfos[primIndex].vbSrvIndex = prim.rtVertices->srvIndex;
        primInfos[primIndex].ibSrvIndex = prim.rtIndices->srvIndex;
        primInfos[primIndex].materialIdx = primMaterialIdx[primIndex];

        geom.Triangles.IndexCount = prim.indexCount;
        geom.Triangles.VertexCount = prim.vertexCount;
        geom.Triangles.IndexBuffer = prim.rtIndices->buffer->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StartAddress = prim.rtVertices->buffer->GetGPUVirtualAddress();

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
        prim.blasScratch = renderer.CreateBuffer(nullptr, d);

        d.sizeInBytes = static_cast<u32>(info.ResultDataMaxSizeInBytes);
        d.name = L"BLAS";
        d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        d.finalState = d.initialState;
        prim.blas = renderer.CreateBuffer(nullptr, d);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData = prim.blas->buffer->GetGPUVirtualAddress();
        build.Inputs = in;
        build.ScratchAccelerationStructureData = prim.blasScratch->buffer->GetGPUVirtualAddress();

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

        m_RTPrimInfoBuffer = renderer.CreateBuffer(nullptr, d);
        renderer.SetBufferData(cmd, m_RTPrimInfoBuffer, primInfos.data(), d.sizeInBytes, 0);
    }

    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.reserve(instances.size());

    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());
        const Primitive& prim = primitives[inst.primIndex];

        D3D12_RAYTRACING_INSTANCE_DESC idesc{};
        idesc.InstanceMask = 0xFF;
        idesc.AccelerationStructure = prim.blas->buffer->GetGPUVirtualAddress();
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
    m_InstanceDescs = renderer.CreateBuffer(nullptr, d);

    {
        u8* mapped = nullptr;
        IE_Check(m_InstanceDescs->buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        std::memcpy(mapped, instanceDescs.data(), d.sizeInBytes);
        m_InstanceDescs->buffer->Unmap(0, nullptr);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.InstanceDescs = m_InstanceDescs->buffer->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topInfo);

    d.heapType = D3D12_HEAP_TYPE_DEFAULT;
    d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    d.finalState = d.initialState;
    d.sizeInBytes = static_cast<u32>(topInfo.ScratchDataSizeInBytes);
    d.name = L"TLAS Scratch";
    m_TlasScratch = renderer.CreateBuffer(nullptr, d);

    d.sizeInBytes = static_cast<u32>(topInfo.ResultDataMaxSizeInBytes);
    d.name = L"TLAS";
    d.initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    d.finalState = d.initialState;
    m_Tlas = renderer.CreateBuffer(nullptr, d);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
    topBuild.DestAccelerationStructureData = m_Tlas->buffer->GetGPUVirtualAddress();
    topBuild.Inputs = topLevelInputs;
    topBuild.ScratchAccelerationStructureData = m_TlasScratch->buffer->GetGPUVirtualAddress();

    cmd->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc{};
    tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tlasSrvDesc.RaytracingAccelerationStructure.Location = m_Tlas->buffer->GetGPUVirtualAddress();
    m_TlasSrvIndex = bindlessHeaps.CreateSRV(nullptr, tlasSrvDesc);
}

void Raytracing::CreateShadowPassResources()
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    const XMUINT2 renderSize = renderer.GetRenderSize();

    const CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const CD3DX12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Shadow.trace.outputTexture)));
    IE_Check(m_Shadow.trace.outputTexture->SetName(L"RT Shadows Output"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc{};
    outputUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Shadow.trace.outputUavIndex = bindlessHeaps.CreateUAV(m_Shadow.trace.outputTexture, outputUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outputSrvDesc{};
    outputSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    outputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outputSrvDesc.Texture2D.MipLevels = 1;
    m_Shadow.trace.outputSrvIndex = bindlessHeaps.CreateSRV(m_Shadow.trace.outputTexture, outputSrvDesc);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Shadow.blur.intermediateResource)));
    IE_Check(m_Shadow.blur.intermediateResource->SetName(L"RTShadows_BlurIntermediate"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC intermediateUavDesc{};
    intermediateUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    intermediateUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Shadow.blur.uavIntermediateIdx = bindlessHeaps.CreateUAV(m_Shadow.blur.intermediateResource, intermediateUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC intermediateSrvDesc = outputSrvDesc;
    m_Shadow.blur.srvRawIdx = m_Shadow.trace.outputSrvIndex;
    m_Shadow.blur.srvIntermediateIdx = bindlessHeaps.CreateSRV(m_Shadow.blur.intermediateResource, intermediateSrvDesc);
}

void Raytracing::CreatePathTracePassResources()
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    const XMUINT2 renderSize = renderer.GetRenderSize();

    const CD3DX12_RESOURCE_DESC outDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const CD3DX12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_PathTrace.trace.indirectDiffuseTexture)));
    IE_Check(m_PathTrace.trace.indirectDiffuseTexture->SetName(L"Indirect Diffuse"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
    outUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_PathTrace.trace.outputUavIndex = bindlessHeaps.CreateUAV(m_PathTrace.trace.indirectDiffuseTexture, outUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
    outSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outSrvDesc.Texture2D.MipLevels = 1;
    m_PathTrace.trace.outputSrvIndex = bindlessHeaps.CreateSRV(m_PathTrace.trace.indirectDiffuseTexture, outSrvDesc);

    constexpr u32 cacheEntries = RC_ENTRIES;
    constexpr u32 cacheByteSize = cacheEntries * sizeof(RadianceCacheEntry);

    BufferCreateDesc d{};
    d.sizeInBytes = cacheByteSize;
    d.heapType = D3D12_HEAP_TYPE_DEFAULT;
    d.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    d.finalState = d.initialState;
    d.viewKind = BufferCreateDesc::ViewKind::Structured;
    d.strideInBytes = sizeof(RadianceCacheEntry);
    d.createSRV = true;
    d.createUAV = true;
    d.name = L"RadianceCache";
    m_PathTrace.trace.radianceCache = renderer.CreateBuffer(nullptr, d);

    const u32 samplesCount = renderSize.x * renderSize.y;
    const u32 samplesByteSize = samplesCount * sizeof(RadianceSample);

    d.sizeInBytes = samplesByteSize;
    d.strideInBytes = sizeof(RadianceSample);
    d.name = L"RadianceSamples";
    m_PathTrace.trace.radianceSamples = renderer.CreateBuffer(nullptr, d);
}

void Raytracing::CreateShadowPassPipelines()
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();

    m_Shadow.trace.shader = IE_LoadShader(IE_SHADER_TYPE_LIB, L"rt/shadows/rtTrace.hlsl", {}, m_Shadow.trace.shader);
    CreateGlobalRootSigConstants(device, sizeof(RtShadowsTraceConstants) / sizeof(u32), m_Shadow.trace.rootSig);

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    auto* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    const D3D12_SHADER_BYTECODE libdxil{m_Shadow.trace.shader->blob->GetBufferPointer(), m_Shadow.trace.shader->blob->GetBufferSize()};
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"AnyHit");
    lib->DefineExport(L"Miss");

    auto* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetAnyHitShaderImport(L"AnyHit");
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

    m_Shadow.blur.csH = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/shadows/csBlurH.hlsl", {}, m_Shadow.blur.csH);
    m_Shadow.blur.csV = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/shadows/csBlurV.hlsl", {}, m_Shadow.blur.csV);

    IE_Check(device->CreateRootSignature(0, m_Shadow.blur.csH->blob->GetBufferPointer(), m_Shadow.blur.csH->blob->GetBufferSize(), IID_PPV_ARGS(&m_Shadow.blur.rootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_Shadow.blur.rootSignature.Get();

    pso.CS = {m_Shadow.blur.csH->blob->GetBufferPointer(), m_Shadow.blur.csH->blob->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_Shadow.blur.horizontalPso)));

    pso.CS = {m_Shadow.blur.csV->blob->GetBufferPointer(), m_Shadow.blur.csV->blob->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_Shadow.blur.verticalPso)));
}

void Raytracing::CreatePathTracePassPipelines()
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();

    m_PathTrace.trace.shader = IE_LoadShader(IE_SHADER_TYPE_LIB, L"rt/pathtrace/rtTrace.hlsl", {}, m_PathTrace.trace.shader);
    CreateGlobalRootSigConstants(device, sizeof(PathTraceConstants) / sizeof(u32), m_PathTrace.trace.rootSig);

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    auto* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    const D3D12_SHADER_BYTECODE libdxil{m_PathTrace.trace.shader->blob->GetBufferPointer(), m_PathTrace.trace.shader->blob->GetBufferSize()};
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"Miss");

    auto* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
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

    m_PathTrace.cache.csClearSamples = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/pathtrace/csClearSamples.hlsl", {}, m_PathTrace.cache.csClearSamples);
    m_PathTrace.cache.csIntegrateSamples = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/pathtrace/csIntegrateSamples.hlsl", {}, m_PathTrace.cache.csIntegrateSamples);
    m_PathTrace.cache.csClearCache = IE_LoadShader(IE_SHADER_TYPE_COMPUTE, L"compute/pathtrace/csClearCache.hlsl", {}, m_PathTrace.cache.csClearCache);

    IE_Check(device->CreateRootSignature(0, m_PathTrace.cache.csClearSamples->blob->GetBufferPointer(), m_PathTrace.cache.csClearSamples->blob->GetBufferSize(),
                                         IID_PPV_ARGS(&m_PathTrace.cache.rootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_PathTrace.cache.rootSignature.Get();

    pso.CS = {m_PathTrace.cache.csClearSamples->blob->GetBufferPointer(), m_PathTrace.cache.csClearSamples->blob->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_PathTrace.cache.clearPso)));

    pso.CS = {m_PathTrace.cache.csIntegrateSamples->blob->GetBufferPointer(), m_PathTrace.cache.csIntegrateSamples->blob->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_PathTrace.cache.integratePso)));

    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso2{};
        pso2.pRootSignature = m_PathTrace.cache.rootSignature.Get();
        pso2.CS = {m_PathTrace.cache.csClearCache->blob->GetBufferPointer(), m_PathTrace.cache.csClearCache->blob->GetBufferSize()};
        IE_Check(device->CreateComputePipelineState(&pso2, IID_PPV_ARGS(&m_PathTrace.cache.clearCachePso)));
    }
}

void Raytracing::ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ShadowPassInput& input)
{
    Renderer& renderer = Renderer::GetInstance();
    Camera& camera = Camera::GetInstance();

    const XMUINT2 renderSize = renderer.GetRenderSize();
    const Camera::FrameData cameraFrameData = camera.GetFrameData();
    Renderer::PerFrameData& frameData = renderer.GetCurrentFrameData();

    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();

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

    const u32 shadowTypeIndex = static_cast<u32>(g_RTShadows_Type);
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
    dispatchRaysDesc.Width = renderSize.x / currentDitherFactors.x;
    dispatchRaysDesc.Height = renderSize.y / currentDitherFactors.y;

    RtShadowsTraceConstants constants{};
    constants.invViewProj = cameraFrameData.invViewProj;
    constants.outputTextureIndex = m_Shadow.trace.outputUavIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(dispatchRaysDesc.Width * currentDitherFactors.x), 1.0f / static_cast<f32>(dispatchRaysDesc.Height * currentDitherFactors.y));
    constants.ditherFactors = currentDitherFactors;
    constants.ditherOffset = ditherOffset;
    constants.sunDir = input.sunDir;
    constants.cameraPos = cameraFrameData.position;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.tlasIndex = m_TlasSrvIndex;

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "RT Shadows");
    {
        renderer.Barrier(cmd, m_Shadow.trace.outputTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_Shadow.trace.rootSig.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetPipelineState1(m_Shadow.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(RtShadowsTraceConstants) / sizeof(u32), &constants, 0);
        cmd->DispatchRays(&dispatchRaysDesc);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "RT Shadows Blur");
    {
        RTShadowsBlurConstants rootConstants{};
        rootConstants.zNear = cameraFrameData.znearfar.x;
        rootConstants.zFar = cameraFrameData.znearfar.y;
        rootConstants.depthTextureIndex = input.depthTextureIndex;

        const u32 dispatchX = IE_DivRoundUp(renderSize.x, 16);
        const u32 dispatchY = IE_DivRoundUp(renderSize.y, 16);

        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_Shadow.blur.rootSignature.Get());

        renderer.UAVBarrier(cmd, m_Shadow.trace.outputTexture);
        renderer.Barrier(cmd, m_Shadow.trace.outputTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer.Barrier(cmd, m_Shadow.blur.intermediateResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetPipelineState(m_Shadow.blur.horizontalPso.Get());
        rootConstants.inputTextureIndex = m_Shadow.blur.srvRawIdx;
        rootConstants.outputTextureIndex = m_Shadow.blur.uavIntermediateIdx;
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        renderer.UAVBarrier(cmd, m_Shadow.blur.intermediateResource);
        renderer.Barrier(cmd, m_Shadow.trace.outputTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer.Barrier(cmd, m_Shadow.blur.intermediateResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmd->SetPipelineState(m_Shadow.blur.verticalPso.Get());
        rootConstants.inputTextureIndex = m_Shadow.blur.srvIntermediateIdx;
        rootConstants.outputTextureIndex = m_Shadow.trace.outputUavIndex;
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        renderer.UAVBarrier(cmd, m_Shadow.trace.outputTexture);
        renderer.Barrier(cmd, m_Shadow.trace.outputTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

void Raytracing::PathTracePass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const PathTracePassInput& input)
{
    Renderer& renderer = Renderer::GetInstance();
    Camera& camera = Camera::GetInstance();

    const XMUINT2 renderSize = renderer.GetRenderSize();
    const Camera::FrameData cameraFrameData = camera.GetFrameData();
    Renderer::PerFrameData& frameData = renderer.GetCurrentFrameData();

    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();

    if (!m_Cleared)
    {
        ClearPathTraceRadianceCacheCS(cmd);
        m_Cleared = true;
    }

    const u32 samplesCount = renderSize.x * renderSize.y;

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Radiance Cache - Clear Samples");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.clearPso.Get());

        renderer.Barrier(cmd, m_PathTrace.trace.radianceSamples->buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheClearSamplesConstants c{};
        c.radianceSamplesUavIndex = m_PathTrace.trace.radianceSamples->uavIndex;
        c.samplesCount = samplesCount;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(samplesCount, kCacheCSGroupSize), 1, 1);

        renderer.UAVBarrier(cmd, m_PathTrace.trace.radianceSamples->buffer);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    const Environment& env = renderer.GetCurrentEnvironment();

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
    constants.indirectDiffuseTextureIndex = m_PathTrace.trace.outputUavIndex;
    constants.sunDir = input.sunDir;
    constants.normalGeoTextureIndex = input.normalGeoTextureIndex;
    constants.fullDimInv = XMFLOAT2(1.0f / static_cast<f32>(renderSize.x), 1.0f / static_cast<f32>(renderSize.y));
    constants.tlasIndex = m_TlasSrvIndex;
    constants.depthTextureIndex = input.depthTextureIndex;
    constants.primInfoBufferIndex = m_RTPrimInfoBuffer->srvIndex;
    constants.materialsBufferIndex = input.materialsBufferIndex;
    constants.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
    constants.radianceCacheSrvIndex = m_PathTrace.trace.radianceCache->srvIndex;
    constants.radianceCacheCellSize = g_RadianceCache_CellSize;
    constants.frameIndex = input.frameIndex;
    constants.radianceSamplesUavIndex = m_PathTrace.trace.radianceSamples->uavIndex;
    constants.samplesCount = samplesCount;
    constants.envMapIndex = env.envSrvIdx;
    constants.skyIntensity = g_IBL_SkyIntensity;
    constants.samplerIndex = input.samplerIndex;
    constants.sunIntensity = g_Sun_Intensity;
    constants.sppCached = g_PathTrace_SppCached;
    constants.sppNotCached = g_PathTrace_SppNotCached;
    constants.bounceCount = g_PathTrace_BounceCount;
    constants.useTrilinear = g_RadianceCache_Trilinear;
    constants.trilinearMinCornerSamples = g_RadianceCache_TrilinearMinCornerSamples;
    constants.trilinearMinHits = g_RadianceCache_TrilinearMinHits;
    constants.trilinearPresentMinSamples = g_RadianceCache_TrilinearPresentMinSamples;
    constants.normalBinRes = g_RadianceCache_NormalBinRes;
    constants.minExtraSppCount = g_RadianceCache_MinExtraSppCount;
    constants.maxAge = g_RadianceCache_MaxAge;
    constants.maxProbes = g_RadianceCache_MaxProbes;
    constants.maxSamples = g_RadianceCache_MaxSamples;
    constants.cellSize = g_RadianceCache_CellSize;

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Path Trace");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());

        renderer.Barrier(cmd, m_PathTrace.trace.indirectDiffuseTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->SetComputeRootSignature(m_PathTrace.trace.rootSig.Get());
        cmd->SetPipelineState1(m_PathTrace.trace.dxrStateObject.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(PathTraceConstants) / sizeof(u32), &constants, 0);

        cmd->DispatchRays(&dispatchRayDesc);

        renderer.UAVBarrier(cmd, m_PathTrace.trace.radianceSamples->buffer);
        renderer.Barrier(cmd, m_PathTrace.trace.indirectDiffuseTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Radiance Cache - Integrate Samples");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.integratePso.Get());

        renderer.Barrier(cmd, m_PathTrace.trace.radianceSamples->buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer.Barrier(cmd, m_PathTrace.trace.radianceCache->buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheIntegrateSamplesConstants c;
        c.radianceSamplesSrvIndex = m_PathTrace.trace.radianceSamples->srvIndex;
        c.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
        c.samplesCount = samplesCount;
        c.frameIndex = input.frameIndex;
        c.maxAge = g_RadianceCache_MaxAge;
        c.maxProbes = g_RadianceCache_MaxProbes;
        c.maxSamples = g_RadianceCache_MaxSamples;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(samplesCount, kCacheCSGroupSize), 1, 1);

        renderer.UAVBarrier(cmd, m_PathTrace.trace.radianceCache->buffer);

        renderer.Barrier(cmd, m_PathTrace.trace.radianceCache->buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer.Barrier(cmd, m_PathTrace.trace.radianceSamples->buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

const Raytracing::PathTracePassResources& Raytracing::GetPathTracePassResources() const
{
    return m_PathTrace;
}

const Raytracing::ShadowPassResources& Raytracing::GetShadowPassResources() const
{
    return m_Shadow;
}

void Raytracing::ClearPathTraceRadianceCacheCS(const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    Renderer& renderer = Renderer::GetInstance();
    Renderer::PerFrameData& frameData = renderer.GetCurrentFrameData();

    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();

    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Radiance Cache - Clear Cache");
    {
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_PathTrace.cache.rootSignature.Get());
        cmd->SetPipelineState(m_PathTrace.cache.clearCachePso.Get());

        renderer.Barrier(cmd, m_PathTrace.trace.radianceCache->buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PathTraceCacheClearCacheConstants c;
        c.radianceCacheUavIndex = m_PathTrace.trace.radianceCache->uavIndex;
        c.cacheEntries = RC_ENTRIES;

        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(u32), &c, 0);
        cmd->Dispatch(IE_DivRoundUp(RC_ENTRIES, kCacheCSGroupSize), 1, 1);

        renderer.UAVBarrier(cmd, m_PathTrace.trace.radianceCache->buffer);
        renderer.Barrier(cmd, m_PathTrace.trace.radianceCache->buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}
