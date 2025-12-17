// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneLoader.h"

#include "Primitive.h"
#include "Raytracing.h"
#include "Renderer.h"
#include "SceneFileLoader.h"
#include "common/Asserts.h"
#include "common/IskurPackFormat.h"
#include "window/Window.h"

#include <DDSTextureLoader.h>
#include <ResourceUploadBatch.h>
#include <meshoptimizer.h>

#include <filesystem>

using namespace DirectX;
namespace fs = std::filesystem;

void SceneLoader::Load(Renderer& renderer, const String& sceneFile)
{
    const String scenePath = String("data/scenes/") + sceneFile + ".glb";
    const fs::path fsScenePath(scenePath.data());
    const fs::path baseDir = fsScenePath.parent_path();
    const fs::path packPath = baseDir / (fsScenePath.stem().wstring() + L".iskurpack");

    SceneFileData sceneData = LoadSceneFile(packPath);

    Renderer::PerFrameData& frameData = renderer.GetCurrentFrameData();

    ComPtr<ID3D12GraphicsCommandList7> cmd;
    IE_Check(renderer.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameData.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));

    LoadTextures(renderer, sceneData);
    LoadSamplers(renderer, sceneData);
    LoadMaterials(renderer, sceneData, cmd);
    BuildPrimitives(renderer, sceneData, cmd);

    // Build raster buckets & RT instances from IEPack instances
    Vector<Raytracing::RTInstance> rtInstances;
    rtInstances.reserve(sceneData.instances.size());

    for (const IEPack::InstanceRecord& inst : sceneData.instances)
    {
        const u32 localPrimId = inst.primIndex;
        IE_Assert(localPrimId < renderer.m_Primitives.size());

        const Material& mat = renderer.m_Materials[inst.materialIndex];
        const AlphaMode am = static_cast<AlphaMode>(mat.alphaMode);
        const CullMode cm = mat.doubleSided ? CullMode_None : CullMode_Back;

        const Primitive& prim = renderer.m_Primitives[localPrimId];

        XMMATRIX Mworld = XMLoadFloat4x4(&inst.world);
        XMMATRIX MworldInv = XMMatrixInverse(nullptr, Mworld);
        XMFLOAT4X4 worldIT{};
        XMStoreFloat4x4(&worldIT, MworldInv);

        PrimitiveConstants pc{};
        pc.world = inst.world;
        pc.worldIT = worldIT;
        pc.meshletCount = prim.meshletCount;
        pc.materialIdx = inst.materialIndex;
        pc.verticesBufferIndex = prim.vertices->srvIndex;
        pc.meshletsBufferIndex = prim.meshlets->srvIndex;
        pc.meshletVerticesBufferIndex = prim.mlVerts->srvIndex;
        pc.meshletTrianglesBufferIndex = prim.mlTris->srvIndex;
        pc.meshletBoundsBufferIndex = prim.mlBounds->srvIndex;
        pc.materialsBufferIndex = renderer.m_MaterialsBuffer->srvIndex;

        Renderer::PrimitiveRenderData prd{};
        prd.primIndex = localPrimId;
        prd.primConstants = pc;
        renderer.m_PrimitivesRenderData[am][cm].push_back(prd);

        Raytracing::RTInstance rti{};
        rti.primIndex = localPrimId;
        rti.materialIndex = pc.materialIdx;
        rti.world = pc.world;
        rtInstances.push_back(rti);
    }

    SetupDepthResourcesAndLinearSampler(renderer);

    Raytracing::GetInstance().Init(cmd, renderer.m_Primitives, rtInstances);

    SubmitAndSync(renderer, frameData, cmd);
}

void SceneLoader::LoadTextures(Renderer& renderer, const SceneFileData& scene)
{
    const auto& texTable = scene.texTable;
    if (texTable.empty())
    {
        renderer.m_TxhdToSrv.clear();
        renderer.m_Textures.clear();
        return;
    }

    const u32 texCount = static_cast<u32>(texTable.size());
    renderer.m_TxhdToSrv.resize(texCount);
    renderer.m_Textures.resize(texCount);

    IE_Assert(!scene.texBlob.empty());
    const u8* texBlob = scene.texBlob.data();

    ResourceUploadBatch batch(renderer.GetDevice().Get());
    batch.Begin();

    for (u32 i = 0; i < texCount; ++i)
    {
        const auto& tr = texTable[i];
        const u8* ddsPtr = texBlob + static_cast<size_t>(tr.byteOffset);

        ComPtr<ID3D12Resource> res;
        IE_Check(CreateDDSTextureFromMemory(renderer.GetDevice().Get(), batch, ddsPtr, tr.byteSize, &res, false, 0, nullptr, nullptr));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = res->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = UINT32_MAX;

        const u32 srvIdx = renderer.m_BindlessHeaps.CreateSRV(res, srv);
        renderer.m_TxhdToSrv[i] = srvIdx;
        renderer.m_Textures[i] = res;
    }

    batch.End(renderer.m_CommandQueue.Get()).wait();
}

void SceneLoader::LoadSamplers(Renderer& renderer, const SceneFileData& scene)
{
    const auto& sampTable = scene.samplers;
    const u32 sampCount = static_cast<u32>(sampTable.size());
    renderer.m_SampToHeap.resize(sampCount);

    for (u32 i = 0; i < sampCount; ++i)
    {
        renderer.m_SampToHeap[i] = renderer.m_BindlessHeaps.CreateSampler(sampTable[i]);
    }
}

void SceneLoader::LoadMaterials(Renderer& renderer, const SceneFileData& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    const auto& matlTable = scene.materials;
    const u32 matCount = static_cast<u32>(matlTable.size());
    renderer.m_Materials.resize(matCount);

    for (u32 i = 0; i < matCount; ++i)
    {
        const auto& mr = matlTable[i];
        Material m;
        m.baseColorFactor = {mr.baseColorFactor[0], mr.baseColorFactor[1], mr.baseColorFactor[2], mr.baseColorFactor[3]};
        m.metallicFactor = mr.metallicFactor;
        m.roughnessFactor = mr.roughnessFactor;
        m.normalScale = mr.normalScale;
        m.alphaCutoff = mr.alphaCutoff;

        // BLEND not supported -> treat as mask
        if (mr.flags & IEPack::MATF_ALPHA_BLEND || mr.flags & IEPack::MATF_ALPHA_MASK)
        {
            m.alphaMode = AlphaMode_Mask;
        }
        else
        {
            m.alphaMode = AlphaMode_Opaque;
        }
        m.doubleSided = !!(mr.flags & IEPack::MATF_DOUBLE_SIDED);

        auto mapTex = [&](int txhdIdx) -> i32 {
            if (txhdIdx < 0)
                return -1;
            IE_Assert(static_cast<size_t>(txhdIdx) < renderer.m_TxhdToSrv.size());
            return static_cast<i32>(renderer.m_TxhdToSrv[static_cast<u32>(txhdIdx)]);
        };
        auto mapSamp = [&](u32 sampIdx, int txhdIdx) -> i32 {
            if (txhdIdx < 0 || sampIdx == UINT32_MAX)
                return -1;
            IE_Assert(static_cast<size_t>(sampIdx) < renderer.m_SampToHeap.size());
            return static_cast<i32>(renderer.m_SampToHeap[sampIdx]);
        };

        m.baseColorTextureIndex = mapTex(mr.baseColorTx);
        m.baseColorSamplerIndex = mapSamp(mr.baseColorSampler, mr.baseColorTx);
        m.metallicRoughnessTextureIndex = mapTex(mr.metallicRoughTx);
        m.metallicRoughnessSamplerIndex = mapSamp(mr.metallicRoughSampler, mr.metallicRoughTx);
        m.normalTextureIndex = mapTex(mr.normalTx);
        m.normalSamplerIndex = mapSamp(mr.normalSampler, mr.normalTx);
        m.aoTextureIndex = mapTex(mr.occlusionTx);
        m.aoSamplerIndex = mapSamp(mr.occlusionSampler, mr.occlusionTx);

        renderer.m_Materials[i] = m;
    }

    if (!renderer.m_Materials.empty())
    {
        const u32 byteSize = static_cast<u32>(renderer.m_Materials.size() * sizeof(Material));

        BufferCreateDesc bufferDesc{};
        bufferDesc.heapType = D3D12_HEAP_TYPE_DEFAULT;
        bufferDesc.viewKind = BufferCreateDesc::ViewKind::Structured;
        bufferDesc.createSRV = true;
        bufferDesc.createUAV = false;
        bufferDesc.resourceFlags = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc.initialState = D3D12_RESOURCE_STATE_COMMON;
        bufferDesc.finalState = bufferDesc.initialState;
        bufferDesc.sizeInBytes = byteSize;
        bufferDesc.strideInBytes = sizeof(Material);
        bufferDesc.name = L"Materials";
        renderer.m_MaterialsBuffer = renderer.CreateBuffer(nullptr, bufferDesc);

        renderer.SetBufferData(cmd, renderer.m_MaterialsBuffer, renderer.m_Materials.data(), byteSize, 0);
    }
}

void SceneLoader::BuildPrimitives(Renderer& renderer, const SceneFileData& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    const auto& prims = scene.prims;
    renderer.m_Primitives.clear();
    renderer.m_Primitives.reserve(prims.size());

    if (prims.empty())
        return;

    const u8* vertBase = scene.vertBlob.data();
    const u8* idxBase = scene.idxBlob.data();
    const u8* mshlBase = scene.mshlBlob.data();
    const u8* mlvtBase = scene.mlvtBlob.data();
    const u8* mltrBase = scene.mltrBlob.data();
    const u8* mlbdBase = scene.mlbdBlob.data();

    const u32 primCount = static_cast<u32>(prims.size());
    for (u32 primId = 0; primId < primCount; ++primId)
    {
        const auto& r = prims[primId];

        const auto* vtx = reinterpret_cast<const Vertex*>(vertBase + r.vertexByteOffset);
        const auto* idx = reinterpret_cast<const u32*>(idxBase + r.indexByteOffset);
        const auto* mlt = reinterpret_cast<const Meshlet*>(mshlBase + r.meshletsByteOffset);
        const auto* mlv = reinterpret_cast<const u32*>(mlvtBase + r.mlVertsByteOffset);
        const auto* mltb = mltrBase + r.mlTrisByteOffset;
        const auto* mlb = reinterpret_cast<const meshopt_Bounds*>(mlbdBase + r.mlBoundsByteOffset);

        Primitive prim{};
        prim.materialIdx = r.materialIndex;
        prim.meshletCount = r.meshletCount;

        // Structured
        BufferCreateDesc bufferDesc{};
        bufferDesc.heapType = D3D12_HEAP_TYPE_DEFAULT;
        bufferDesc.viewKind = BufferCreateDesc::ViewKind::Structured;
        bufferDesc.createSRV = true;
        bufferDesc.createUAV = false;
        bufferDesc.resourceFlags = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc.initialState = D3D12_RESOURCE_STATE_COMMON;
        bufferDesc.finalState = bufferDesc.initialState;
        bufferDesc.sizeInBytes = r.vertexCount * sizeof(Vertex);
        bufferDesc.strideInBytes = sizeof(Vertex);
        bufferDesc.name = L"SceneLoader/Vertices";
        prim.vertices = renderer.CreateBuffer(nullptr, bufferDesc);
        renderer.SetBufferData(cmd, prim.vertices, vtx, r.vertexCount * sizeof(Vertex), 0);

        bufferDesc.sizeInBytes = r.mlVertsCount * sizeof(u32);
        bufferDesc.strideInBytes = sizeof(u32);
        bufferDesc.name = L"SceneLoader/MeshletVerts";
        prim.mlVerts = renderer.CreateBuffer(nullptr, bufferDesc);
        renderer.SetBufferData(cmd, prim.mlVerts, mlv, r.mlVertsCount * sizeof(u32), 0);

        bufferDesc.sizeInBytes = r.meshletCount * sizeof(meshopt_Bounds);
        bufferDesc.strideInBytes = sizeof(meshopt_Bounds);
        bufferDesc.name = L"SceneLoader/MeshletBounds";
        prim.mlBounds = renderer.CreateBuffer(nullptr, bufferDesc);
        renderer.SetBufferData(cmd, prim.mlBounds, mlb, r.meshletCount * sizeof(meshopt_Bounds), 0);

        // Raw
        bufferDesc.viewKind = BufferCreateDesc::ViewKind::Raw;
        bufferDesc.sizeInBytes = r.meshletCount * sizeof(Meshlet);
        bufferDesc.name = L"SceneLoader/Meshlets";
        prim.meshlets = renderer.CreateBuffer(nullptr, bufferDesc);
        renderer.SetBufferData(cmd, prim.meshlets, mlt, r.meshletCount * sizeof(Meshlet), 0);

        bufferDesc.sizeInBytes = r.mlTrisByteCount * sizeof(u32);
        bufferDesc.name = L"SceneLoader/MeshletTris";
        prim.mlTris = renderer.CreateBuffer(nullptr, bufferDesc);
        renderer.SetBufferData(cmd, prim.mlTris, mltb, r.mlTrisByteCount, 0);

        // CPU data for RT BLAS
        prim.cpuVertices = vtx;
        prim.vertexCount = r.vertexCount;
        prim.cpuIndices = idx;
        prim.indexCount = r.indexCount;

        renderer.m_Primitives.push_back(std::move(prim));
    }
}

void SceneLoader::SetupDepthResourcesAndLinearSampler(Renderer& renderer)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        renderer.m_DepthPre.dsvSrvIdx[i] = renderer.m_BindlessHeaps.CreateSRV(renderer.m_DepthPre.dsvs[i], srvDesc);
    }

    D3D12_SAMPLER_DESC linearClampDesc{};
    linearClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearClampDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampDesc.MaxAnisotropy = 1;
    linearClampDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    linearClampDesc.MaxLOD = D3D12_FLOAT32_MAX;

    renderer.m_LinearSamplerIdx = renderer.m_BindlessHeaps.CreateSampler(linearClampDesc);
}

void SceneLoader::SubmitAndSync(Renderer& renderer, Renderer::PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    IE_Check(cmd->Close());
    ID3D12CommandList* pCmd = cmd.Get();
    renderer.m_CommandQueue->ExecuteCommandLists(1, &pCmd);

    const u64 fenceToWait = ++frameData.frameFenceValue;
    IE_Check(renderer.m_CommandQueue->Signal(frameData.frameFence.Get(), fenceToWait));

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    IE_Check(frameData.frameFence->SetEventOnCompletion(fenceToWait, evt));
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
}
