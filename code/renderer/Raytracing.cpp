// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Raytracing.h"

#include "Camera.h"

#include "CompileShader.h"
#include "ImGui.h"
#include "Renderer.h"
#include "Shader.h"

void Raytracing::Init(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances)
{
    InitRaytracingWorld(cmd, primitives, instances);
    InitRaytracingShadows();
}

void Raytracing::Terminate()
{
}

void Raytracing::InitRaytracingWorld(ComPtr<ID3D12GraphicsCommandList7>& cmd, Vector<Primitive>& primitives, const Vector<RTInstance>& instances)
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();

    D3D12_RAYTRACING_GEOMETRY_DESC geom;
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.Transform3x4 = 0;
    geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = 1;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    for (Primitive& prim : primitives)
    {
        renderer.AllocateUploadBuffer(prim.cpuVertices, prim.vertexCount * sizeof(Vertex), 0, prim.rtVB, prim.rtVBAlloc, L"PackRT/VB");
        renderer.AllocateUploadBuffer(prim.cpuIndices, prim.indexCount * sizeof(u32), 0, prim.rtIB, prim.rtIBAlloc, L"PackRT/IB");

        geom.Triangles.IndexCount = prim.indexCount;
        geom.Triangles.VertexCount = prim.vertexCount;
        geom.Triangles.IndexBuffer = prim.rtIB->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StartAddress = prim.rtVB->GetGPUVirtualAddress();

        in.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

        renderer.AllocateUAVBuffer(static_cast<u32>(info.ResultDataMaxSizeInBytes), prim.blas, prim.blasAlloc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS");
        renderer.AllocateUAVBuffer(static_cast<u32>(info.ScratchDataSizeInBytes), prim.scratch, prim.scratchAlloc, D3D12_RESOURCE_STATE_COMMON, L"BLAS Scratch");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData = prim.blas->GetGPUVirtualAddress();
        build.Inputs = in;
        build.ScratchAccelerationStructureData = prim.scratch->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    }

    D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmd->ResourceBarrier(1, &uav);

    // Build instance list (TLAS)
    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.reserve(instances.size());
    for (const RTInstance& inst : instances)
    {
        IE_Assert(inst.primIndex < primitives.size());
        const Primitive& prim = primitives[inst.primIndex];

        D3D12_RAYTRACING_INSTANCE_DESC idesc{};
        idesc.InstanceMask = 1;
        idesc.AccelerationStructure = prim.blas->GetGPUVirtualAddress();

        // row-major XMFLOAT4X4 -> 3x4 expected by DXR (row-major)
        idesc.Transform[0][0] = inst.world._11;
        idesc.Transform[0][1] = inst.world._21;
        idesc.Transform[0][2] = inst.world._31;
        idesc.Transform[0][3] = inst.world._41;
        idesc.Transform[1][0] = inst.world._12;
        idesc.Transform[1][1] = inst.world._22;
        idesc.Transform[1][2] = inst.world._32;
        idesc.Transform[1][3] = inst.world._42;
        idesc.Transform[2][0] = inst.world._13;
        idesc.Transform[2][1] = inst.world._23;
        idesc.Transform[2][2] = inst.world._33;
        idesc.Transform[2][3] = inst.world._43;

        instanceDescs.push_back(idesc);
    }

    renderer.AllocateUploadBuffer(instanceDescs.data(), static_cast<u32>(instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)), 0, m_InstanceDescs, m_InstanceDescsAlloc, L"InstanceDescs");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.InstanceDescs = m_InstanceDescs->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topInfo);

    renderer.AllocateUAVBuffer(static_cast<u32>(topInfo.ScratchDataSizeInBytes), m_ScratchResource, m_ScratchResourceAlloc, D3D12_RESOURCE_STATE_COMMON, L"ScratchResource");
    renderer.AllocateUAVBuffer(static_cast<u32>(topInfo.ResultDataMaxSizeInBytes), m_TLAS, m_TLASAlloc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
    topBuild.DestAccelerationStructureData = m_TLAS->GetGPUVirtualAddress();
    topBuild.Inputs = topLevelInputs;
    topBuild.ScratchAccelerationStructureData = m_ScratchResource->GetGPUVirtualAddress();
    cmd->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);

    // Bindless SRV for TLAS
    D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc{};
    tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tlasSrvDesc.RaytracingAccelerationStructure.Location = m_TLAS->GetGPUVirtualAddress();
    m_Constants.tlasIndex = bindlessHeaps.CreateSRV(nullptr, tlasSrvDesc);
}

void Raytracing::InitRaytracingShadows()
{
    Renderer& renderer = Renderer::GetInstance();
    const ComPtr<ID3D12Device14>& device = renderer.GetDevice();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    XMUINT2 renderSize = renderer.GetRenderSize();

    // Create 2D output texture for raytracing.
    Shader raytracingShader = Renderer::LoadShader(IE_SHADER_TYPE_LIB, L"rtShadows.hlsl", {});

    // Create the output resource
    CD3DX12_RESOURCE_DESC outputResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    IE_Check(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &outputResourceDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&m_ShadowPassOutput.resource)));
    IE_Check(m_ShadowPassOutput.resource->SetName(L"RT Shadows Output"));

    // UAV (for DXR + blur writes)
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc{};
    outputUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_ShadowPassOutput.uavIndex = bindlessHeaps.CreateUAV(m_ShadowPassOutput.resource, outputUavDesc);

    // SRV (for lighting pass reads)
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc{};
    shadowSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDesc.Texture2D.MipLevels = 1;
    m_ShadowPassOutput.srvIndex = bindlessHeaps.CreateSRV(m_ShadowPassOutput.resource, shadowSrvDesc);

    CD3DX12_ROOT_PARAMETER rootParameter;
    rootParameter.InitAsConstants(sizeof(RtShadowsTraceConstants) / sizeof(u32), 0);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(1, &rootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSignatureDesc(rsDesc);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    IE_Check(D3D12SerializeVersionedRootSignature(&globalRootSignatureDesc, &blob, &error));
    IE_Check(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RaytracingGlobalRootSignature)));

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

    D3D12_SHADER_BYTECODE libdxil{raytracingShader.bytecode.pShaderBytecode, raytracingShader.bytecode.BytecodeLength};
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"AnyHit");
    lib->DefineExport(L"Miss");

    CD3DX12_HIT_GROUP_SUBOBJECT* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetAnyHitShaderImport(L"AnyHit");
    hitGroup->SetHitGroupExport(L"HitGroup");
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* shaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    u32 payloadSize = 1 * sizeof(u32);   // struct RayPayload is just one uint
    u32 attributeSize = 2 * sizeof(f32); // BuiltInTriangleIntersectionAttributes
    shaderConfig->Config(payloadSize, attributeSize);

    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_RaytracingGlobalRootSignature.Get());

    CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    constexpr u32 maxRecursionDepth = 1;
    pipelineConfig->Config(maxRecursionDepth);

    IE_Check(device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_DxrStateObject)));

    ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    IE_Check(m_DxrStateObject.As(&stateObjectProperties));

    u32 shaderRecordSize = (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1)) & ~(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(shaderRecordSize);
    CD3DX12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto createShaderTable = [&](const wchar_t* name, const wchar_t* exportName, ComPtr<ID3D12Resource>& out) {
        IE_Check(device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)));
        IE_Check(out->SetName(name));
        renderer.SetResourceBufferData(out, stateObjectProperties->GetShaderIdentifier(exportName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);
    };
    createShaderTable(L"RayGenShaderTable", L"Raygen", m_RayGenShaderTable);
    createShaderTable(L"MissShaderTable", L"Miss", m_MissShaderTable);
    createShaderTable(L"HitGroupShaderTable", L"HitGroup", m_HitGroupShaderTable);

    m_ShadowPassDispatchRaysDesc.RayGenerationShaderRecord.StartAddress = m_RayGenShaderTable->GetGPUVirtualAddress();
    m_ShadowPassDispatchRaysDesc.RayGenerationShaderRecord.SizeInBytes = m_RayGenShaderTable->GetDesc().Width;
    m_ShadowPassDispatchRaysDesc.MissShaderTable.StartAddress = m_MissShaderTable->GetGPUVirtualAddress();
    m_ShadowPassDispatchRaysDesc.MissShaderTable.SizeInBytes = m_MissShaderTable->GetDesc().Width;
    m_ShadowPassDispatchRaysDesc.MissShaderTable.StrideInBytes = m_MissShaderTable->GetDesc().Width;
    m_ShadowPassDispatchRaysDesc.HitGroupTable.StartAddress = m_HitGroupShaderTable->GetGPUVirtualAddress();
    m_ShadowPassDispatchRaysDesc.HitGroupTable.SizeInBytes = m_HitGroupShaderTable->GetDesc().Width;
    m_ShadowPassDispatchRaysDesc.HitGroupTable.StrideInBytes = m_HitGroupShaderTable->GetDesc().Width;
    m_ShadowPassDispatchRaysDesc.Depth = 1;

    // Setup blur pass
    // Intermediate texture
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, renderSize.x, renderSize.y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    IE_Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_Blur.intermediateResource)));
    IE_Check(m_Blur.intermediateResource->SetName(L"RTShadows_BlurIntermediate"));

    D3D12_UNORDERED_ACCESS_VIEW_DESC intermediateUavDesc{};
    intermediateUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    intermediateUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Blur.uavIntermediateIdx = bindlessHeaps.CreateUAV(m_Blur.intermediateResource, intermediateUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC shadowPassOutputSrvDesc{};
    shadowPassOutputSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    shadowPassOutputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowPassOutputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowPassOutputSrvDesc.Texture2D.MipLevels = 1;
    m_Blur.srvRawIdx = m_ShadowPassOutput.srvIndex;
    m_Blur.srvIntermediateIdx = bindlessHeaps.CreateSRV(m_Blur.intermediateResource, shadowPassOutputSrvDesc);

    ComPtr<IDxcBlob> csH = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csBlurH.hlsl", {});
    ComPtr<IDxcBlob> csV = CompileShader(IE_SHADER_TYPE_COMPUTE, L"csBlurV.hlsl", {});

    IE_Check(device->CreateRootSignature(0, csH->GetBufferPointer(), csH->GetBufferSize(), IID_PPV_ARGS(&m_Blur.rootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_Blur.rootSignature.Get();
    pso.CS = {csH->GetBufferPointer(), csH->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_Blur.horizontalPso)));
    pso.pRootSignature = m_Blur.rootSignature.Get();
    pso.CS = {csV->GetBufferPointer(), csV->GetBufferSize()};
    IE_Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_Blur.verticalPso)));
}

void Raytracing::ShadowPass(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const ShadowPassInput& input)
{
    Renderer& renderer = Renderer::GetInstance();
    Camera& camera = Camera::GetInstance();
    XMUINT2 renderSize = renderer.GetRenderSize();
    Camera::FrameData cameraFrameData = camera.GetFrameData();
    Renderer::PerFrameData& frameData = renderer.GetCurrentFrameData();
    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();
    Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = bindlessHeaps.GetDescriptorHeaps();

    // Dithering / tiling
    constexpr XMUINT2 ditherFactors[4] = {
        XMUINT2(1, 1), // full
        XMUINT2(1, 2), // fullX_halfY
        XMUINT2(2, 2), // half
        XMUINT2(4, 4)  // quarter
    };
    constexpr u32 invBayer2[4] = {0, 3, 1, 2};
    constexpr u32 invBayer4[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
    constexpr u32 rtTileCount[4] = {
        1, // full
        2, // fullX_halfY
        4, // half
        16 // quarter
    };

    u32 shadowTypeIndex = static_cast<u32>(g_RTShadows_Type);
    XMUINT2 currentDitherFactors = ditherFactors[shadowTypeIndex];

    u32 tileCount = rtTileCount[shadowTypeIndex];
    u32 slot = input.frameIndex % tileCount;

    u32 idx;
    if (tileCount < 4)
    {
        idx = slot;
    }
    else if (tileCount == 4)
    {
        idx = invBayer2[slot];
    }
    else
    {
        idx = invBayer4[slot];
    }

    u32 shift = currentDitherFactors.x >> 1;
    u32 mask = currentDitherFactors.x - 1;
    XMUINT2 ditherOffset = XMUINT2(idx & mask, idx >> shift);

    // Shadow resolution
    m_ShadowPassDispatchRaysDesc.Width = renderSize.x / currentDitherFactors.x;
    m_ShadowPassDispatchRaysDesc.Height = renderSize.y / currentDitherFactors.y;

    // Shadows RayTracing
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Shadows Ray-Tracing");
    {
        renderer.Barrier(cmd, m_ShadowPassOutput.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(m_RaytracingGlobalRootSignature.Get());
        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetPipelineState1(m_DxrStateObject.Get());
        m_Constants.invViewProj = cameraFrameData.invViewProj;
        m_Constants.outputTextureIndex = m_ShadowPassOutput.uavIndex;
        m_Constants.fullDimInv =
            XMFLOAT2(1.0f / static_cast<float>(m_ShadowPassDispatchRaysDesc.Width * currentDitherFactors.x), 1.0f / static_cast<float>(m_ShadowPassDispatchRaysDesc.Height * currentDitherFactors.y));
        m_Constants.ditherFactors = currentDitherFactors;
        m_Constants.ditherOffset = ditherOffset;
        m_Constants.sunDir = input.sunDir;
        m_Constants.cameraPos = cameraFrameData.position;
        m_Constants.depthTextureIndex = input.depthTextureIndex;
        cmd->SetComputeRoot32BitConstants(0, sizeof(RtShadowsTraceConstants) / sizeof(u32), &m_Constants, 0);
        cmd->DispatchRays(&m_ShadowPassDispatchRaysDesc);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);

    // Blur RT Shadows
    GPU_MARKER_BEGIN(cmd, frameData.gpuTimers, "Ray-Traced Shadows Blur");
    {
        RTShadowsBlurConstants rootConstants;
        rootConstants.zNear = cameraFrameData.znearfar.x;
        rootConstants.zFar = cameraFrameData.znearfar.y;
        rootConstants.depthTextureIndex = input.depthTextureIndex;

        u32 dispatchX = IE_DivRoundUp(renderSize.x, 16);
        u32 dispatchY = IE_DivRoundUp(renderSize.y, 16);

        cmd->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        cmd->SetComputeRootSignature(m_Blur.rootSignature.Get());

        renderer.UAVBarrier(cmd, m_ShadowPassOutput.resource);
        renderer.Barrier(cmd, m_ShadowPassOutput.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer.Barrier(cmd, m_Blur.intermediateResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Horizontal pass
        cmd->SetPipelineState(m_Blur.horizontalPso.Get());
        rootConstants.inputTextureIndex = m_Blur.srvRawIdx;
        rootConstants.outputTextureIndex = m_Blur.uavIntermediateIdx;
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        renderer.UAVBarrier(cmd, m_Blur.intermediateResource);
        renderer.Barrier(cmd, m_ShadowPassOutput.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer.Barrier(cmd, m_Blur.intermediateResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Vertical pass
        cmd->SetPipelineState(m_Blur.verticalPso.Get());
        rootConstants.inputTextureIndex = m_Blur.srvIntermediateIdx;
        rootConstants.outputTextureIndex = m_ShadowPassOutput.uavIndex;
        cmd->SetComputeRoot32BitConstants(0, sizeof(RTShadowsBlurConstants) / sizeof(u32), &rootConstants, 0);
        cmd->Dispatch(dispatchX, dispatchY, 1);

        renderer.UAVBarrier(cmd, m_ShadowPassOutput.resource);
        renderer.Barrier(cmd, m_ShadowPassOutput.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GPU_MARKER_END(cmd, frameData.gpuTimers);
}

const Raytracing::ShadowPassOutput& Raytracing::GetShadowPassOutput() const
{
    return m_ShadowPassOutput;
}