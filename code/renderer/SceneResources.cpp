// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "SceneResources.h"

#include <DirectXTex.h>

#include "BindlessHeaps.h"
#include "RenderDevice.h"
#include "SceneLoader.h"
#include "SceneUtils.h"

SceneResources::SceneResources(RenderDevice& renderDevice, BindlessHeaps& bindlessHeaps) : m_RenderDevice(renderDevice), m_BindlessHeaps(bindlessHeaps)
{
}

void SceneResources::Reset()
{
    m_DefaultWrapSamplerIdx = UINT_MAX;
    m_LinearSamplerIdx = UINT_MAX;

    m_TxhdToSrv.clear();
    m_SampToHeap.clear();
    m_Textures.clear();
    m_Materials.clear();
    m_MaterialsBuffer.reset();
    m_Primitives.clear();
    m_Instances.clear();
    m_InstancesDirty = true;
}

void SceneResources::ImportScene(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    ImportSceneTextures(scene, cmd);
    ImportSceneSamplers(scene);
    CreateDefaultSamplers();
    ImportSceneMaterials(scene, cmd);
    ImportScenePrimitives(scene, cmd);
    ImportSceneInstances(scene);
}

void SceneResources::RefreshAvailableScenes()
{
    m_AvailableScenes = SceneUtils::EnumerateAvailableScenes();
    m_CurrentSceneFile = SceneUtils::ResolveSceneNameFromList(m_CurrentSceneFile, m_AvailableScenes);

    if (!m_PendingSceneFile.empty())
    {
        const String resolvedPending = SceneUtils::ResolveSceneNameFromList(m_PendingSceneFile, m_AvailableScenes);

        bool found = false;
        for (const String& scene : m_AvailableScenes)
        {
            if (SceneUtils::EqualsIgnoreCaseAscii(scene, resolvedPending))
            {
                m_PendingSceneFile = scene;
                found = true;
                break;
            }
        }

        if (!found)
        {
            IE_LogWarn("Pending scene '{}' no longer exists, clearing pending switch", m_PendingSceneFile);
            m_PendingSceneFile.clear();
        }
    }
}

void SceneResources::RequestSceneSwitch(const String& sceneFile)
{
    const String resolvedScene = ResolveSceneName(sceneFile);
    if (resolvedScene.empty())
    {
        return;
    }

    if (SceneUtils::EqualsIgnoreCaseAscii(resolvedScene, m_CurrentSceneFile))
    {
        return;
    }

    if (SceneUtils::EqualsIgnoreCaseAscii(resolvedScene, m_PendingSceneFile))
    {
        return;
    }

    m_PendingSceneFile = resolvedScene;
    IE_LogInfo("Queued scene switch to '{}'", resolvedScene);
}

String SceneResources::ResolveSceneName(const String& sceneFile) const
{
    return SceneUtils::ResolveSceneNameFromList(sceneFile, m_AvailableScenes);
}

bool SceneResources::HasPendingSceneSwitch() const
{
    return !m_PendingSceneFile.empty();
}

const String& SceneResources::GetCurrentSceneFile() const
{
    return m_CurrentSceneFile;
}

const String& SceneResources::GetPendingSceneFile() const
{
    return m_PendingSceneFile;
}

const Vector<String>& SceneResources::GetAvailableScenes() const
{
    return m_AvailableScenes;
}

void SceneResources::SetCurrentSceneFile(const String& sceneFile)
{
    m_CurrentSceneFile = sceneFile;
}

void SceneResources::ClearPendingSceneSwitch()
{
    m_PendingSceneFile.clear();
}

const SharedPtr<Buffer>& SceneResources::GetMaterialsBuffer() const
{
    return m_MaterialsBuffer;
}

const Vector<Material>& SceneResources::GetMaterials() const
{
    return m_Materials;
}

const Vector<Primitive>& SceneResources::GetPrimitives() const
{
    return m_Primitives;
}

Vector<Primitive>& SceneResources::GetPrimitives()
{
    return m_Primitives;
}

const Vector<InstanceData>& SceneResources::GetInstances() const
{
    return m_Instances;
}

Vector<InstanceData>& SceneResources::GetInstances()
{
    return m_Instances;
}

Vector<Raytracing::RTInstance> SceneResources::BuildRTInstances() const
{
    Vector<Raytracing::RTInstance> rtInstances;
    rtInstances.reserve(m_Instances.size());

    for (const InstanceData& instance : m_Instances)
    {
        IE_Assert(instance.materialIndex < m_Materials.size());

        Raytracing::RTInstance rtInstance{};
        rtInstance.primIndex = instance.primIndex;
        rtInstance.materialIndex = instance.materialIndex;
        rtInstance.alphaMode = m_Materials[instance.materialIndex].alphaMode;
        rtInstance.world = instance.world;
        rtInstances.push_back(rtInstance);
    }

    return rtInstances;
}

u32 SceneResources::GetLinearSamplerIdx() const
{
    return m_LinearSamplerIdx;
}

bool SceneResources::AreInstancesDirty() const
{
    return m_InstancesDirty;
}

void SceneResources::MarkInstancesDirty()
{
    m_InstancesDirty = true;
}

void SceneResources::ClearInstancesDirty()
{
    m_InstancesDirty = false;
}

void SceneResources::ImportSceneTextures(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    using namespace DirectX;

    m_Textures.clear();
    m_TxhdToSrv.clear();
    m_Textures.reserve(scene.textures.size());
    m_TxhdToSrv.reserve(scene.textures.size());

    for (const LoadedTexture& src : scene.textures)
    {
        IE_Assert(src.texelBytes != nullptr && src.texelByteCount > 0);
        IE_Assert(src.subresources != nullptr && src.subresourceCount > 0);

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(src.dimension);
        desc.Width = src.width;
        desc.Height = static_cast<UINT>(src.height);
        desc.DepthOrArraySize = (src.dimension == TEX_DIMENSION_TEXTURE3D) ? static_cast<UINT16>(src.depth) : static_cast<UINT16>(src.arraySize);
        desc.MipLevels = static_cast<UINT16>(src.mipLevels);
        desc.Format = static_cast<DXGI_FORMAT>(src.format);
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        Texture texture{};
        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        IE_Check(m_RenderDevice.GetDevice()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource)));
        texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
        texture.SetName(L"Scene Texture");

        Vector<D3D12_SUBRESOURCE_DATA> subresources;
        subresources.reserve(src.subresourceCount);
        for (u32 i = 0; i < src.subresourceCount; ++i)
        {
            const IEPack::TextureSubresourceRecord& packedSubresource = src.subresources[i];
            IE_Assert(static_cast<size_t>(packedSubresource.byteOffset) + static_cast<size_t>(packedSubresource.byteSize) <= src.texelByteCount);

            D3D12_SUBRESOURCE_DATA subresource{};
            subresource.pData = src.texelBytes + static_cast<size_t>(packedSubresource.byteOffset);
            subresource.RowPitch = static_cast<LONG_PTR>(packedSubresource.rowPitch);
            subresource.SlicePitch = static_cast<LONG_PTR>(packedSubresource.slicePitch);
            subresources.push_back(subresource);
        }

        const UINT subresourceCount = static_cast<UINT>(subresources.size());
        const UINT64 uploadSize = GetRequiredIntermediateSize(texture.resource.Get(), 0, subresourceCount);
        RenderDevice::UploadTemp upload{};
        const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        IE_Check(m_RenderDevice.GetDevice()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload.resource)));
        IE_Check(upload.resource->SetName(L"Scene Texture Upload"));

        UpdateSubresources(cmd.Get(), texture.resource.Get(), upload.resource.Get(), 0, 0, subresourceCount, subresources.data());
        texture.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = static_cast<DXGI_FORMAT>(src.format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const bool isCubemap = (src.miscFlags & TEX_MISC_TEXTURECUBE) != 0;
        if (src.dimension == TEX_DIMENSION_TEXTURE1D)
        {
            if (src.arraySize > 1)
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srv.Texture1DArray.ArraySize = static_cast<UINT>(src.arraySize);
                srv.Texture1DArray.MipLevels = static_cast<UINT>(src.mipLevels);
            }
            else
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srv.Texture1D.MipLevels = static_cast<UINT>(src.mipLevels);
            }
        }
        else if (src.dimension == TEX_DIMENSION_TEXTURE3D)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MipLevels = static_cast<UINT>(src.mipLevels);
        }
        else if (isCubemap)
        {
            if (src.arraySize > 6)
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srv.TextureCubeArray.NumCubes = static_cast<UINT>(src.arraySize / 6);
                srv.TextureCubeArray.MipLevels = static_cast<UINT>(src.mipLevels);
            }
            else
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv.TextureCube.MipLevels = static_cast<UINT>(src.mipLevels);
            }
        }
        else if (src.arraySize > 1)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.ArraySize = static_cast<UINT>(src.arraySize);
            srv.Texture2DArray.MipLevels = static_cast<UINT>(src.mipLevels);
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = static_cast<UINT>(src.mipLevels);
        }

        texture.srvIndex = m_BindlessHeaps.CreateSRV(texture.resource, srv);
        m_TxhdToSrv.push_back(texture.srvIndex);
        m_Textures.push_back(std::move(texture));
        m_RenderDevice.TrackUpload(std::move(upload));
    }
}

void SceneResources::ImportSceneSamplers(const LoadedScene& scene)
{
    m_SampToHeap.clear();
    m_SampToHeap.reserve(scene.samplers.size());

    for (const D3D12_SAMPLER_DESC& samplerDesc : scene.samplers)
    {
        m_SampToHeap.push_back(m_BindlessHeaps.CreateSampler(samplerDesc));
    }
}

void SceneResources::CreateDefaultSamplers()
{
    constexpr UINT kMaterialMaxAnisotropy = 16;
    D3D12_SAMPLER_DESC defaultWrapSampler{};
    defaultWrapSampler.Filter = D3D12_FILTER_ANISOTROPIC;
    defaultWrapSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    defaultWrapSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    defaultWrapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    defaultWrapSampler.MinLOD = 0.0f;
    defaultWrapSampler.MaxLOD = D3D12_FLOAT32_MAX;
    defaultWrapSampler.MaxAnisotropy = kMaterialMaxAnisotropy;
    defaultWrapSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    m_DefaultWrapSamplerIdx = m_BindlessHeaps.CreateSampler(defaultWrapSampler);

    D3D12_SAMPLER_DESC linearSampler = defaultWrapSampler;
    linearSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.MaxAnisotropy = 1;
    m_LinearSamplerIdx = m_BindlessHeaps.CreateSampler(linearSampler);
}

void SceneResources::ImportSceneMaterials(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    IE_Assert(m_DefaultWrapSamplerIdx != UINT_MAX);

    m_Materials.clear();
    m_Materials.reserve(scene.materials.size());

    auto mapTextureIndex = [this](i32 sceneTextureIndex) -> i32 {
        if (sceneTextureIndex < 0)
        {
            return -1;
        }

        IE_Assert(static_cast<size_t>(sceneTextureIndex) < m_TxhdToSrv.size());
        return static_cast<i32>(m_TxhdToSrv[sceneTextureIndex]);
    };

    auto mapSamplerIndex = [this](i32 sceneSamplerIndex, i32 sceneTextureIndex) -> i32 {
        if (sceneTextureIndex < 0)
        {
            return -1;
        }

        if (sceneSamplerIndex < 0)
        {
            return static_cast<i32>(m_DefaultWrapSamplerIdx);
        }

        IE_Assert(static_cast<size_t>(sceneSamplerIndex) < m_SampToHeap.size());
        return static_cast<i32>(m_SampToHeap[sceneSamplerIndex]);
    };

    for (const LoadedMaterial& src : scene.materials)
    {
        Material dst{};
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTextureIndex = mapTextureIndex(src.baseColorTextureIndex);
        dst.baseColorSamplerIndex = mapSamplerIndex(src.baseColorSamplerIndex, src.baseColorTextureIndex);
        dst.baseColorFactor = src.baseColorFactor;
        dst.alphaMode = src.alphaMode;
        dst.alphaCutoff = src.alphaCutoff;
        dst.metallicRoughnessTextureIndex = mapTextureIndex(src.metallicRoughnessTextureIndex);
        dst.metallicRoughnessSamplerIndex = mapSamplerIndex(src.metallicRoughnessSamplerIndex, src.metallicRoughnessTextureIndex);
        dst.normalTextureIndex = mapTextureIndex(src.normalTextureIndex);
        dst.normalSamplerIndex = mapSamplerIndex(src.normalSamplerIndex, src.normalTextureIndex);
        dst.normalScale = src.normalScale;
        dst.doubleSided = src.doubleSided;
        dst.aoTextureIndex = mapTextureIndex(src.aoTextureIndex);
        dst.aoSamplerIndex = mapSamplerIndex(src.aoSamplerIndex, src.aoTextureIndex);
        dst.emissiveTextureIndex = mapTextureIndex(src.emissiveTextureIndex);
        dst.emissiveSamplerIndex = mapSamplerIndex(src.emissiveSamplerIndex, src.emissiveTextureIndex);
        dst.emissiveFactor = src.emissiveFactor;
        m_Materials.push_back(dst);
    }

    if (m_Materials.empty())
    {
        return;
    }

    BufferCreateDesc d{};
    d.sizeInBytes = static_cast<u32>(m_Materials.size() * sizeof(Material));
    d.strideInBytes = sizeof(Material);
    d.heapType = D3D12_HEAP_TYPE_DEFAULT;
    d.viewKind = BufferCreateDesc::ViewKind::Structured;
    d.createSRV = true;
    d.createUAV = false;
    d.initialData = m_Materials.data();
    d.initialDataSize = d.sizeInBytes;
    d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;
    d.name = L"Scene Materials";
    m_MaterialsBuffer = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);
}

void SceneResources::ImportScenePrimitives(const LoadedScene& scene, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    m_Primitives.clear();
    m_Primitives.reserve(scene.primitives.size());

    for (const LoadedPrimitive& src : scene.primitives)
    {
        Primitive prim{};
        prim.meshletCount = src.meshletCount;
        prim.localBoundsCenter = src.localBoundsCenter;
        prim.localBoundsRadius = src.localBoundsRadius;

        BufferCreateDesc d{};
        d.heapType = D3D12_HEAP_TYPE_DEFAULT;
        d.createSRV = true;
        d.createUAV = false;
        d.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        d.finalState = D3D12_RESOURCE_STATE_GENERIC_READ;

        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.sizeInBytes = src.vertexCount * sizeof(Vertex);
        d.strideInBytes = sizeof(Vertex);
        d.initialData = src.vertices;
        d.initialDataSize = d.sizeInBytes;
        d.name = L"Primitive/vertices";
        prim.vertices = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);

        d.viewKind = BufferCreateDesc::ViewKind::Raw;
        d.sizeInBytes = src.meshletCount * sizeof(Meshlet);
        d.strideInBytes = 0;
        d.initialData = src.meshlets;
        d.initialDataSize = d.sizeInBytes;
        d.name = L"Primitive/meshlets";
        prim.meshlets = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);

        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.sizeInBytes = src.meshletVertexCount * sizeof(u32);
        d.strideInBytes = sizeof(u32);
        d.initialData = src.meshletVertices;
        d.initialDataSize = d.sizeInBytes;
        d.name = L"Primitive/meshletVertices";
        prim.mlVerts = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);

        d.viewKind = BufferCreateDesc::ViewKind::Raw;
        d.sizeInBytes = IE_AlignUp(src.meshletTriangleByteCount, 4u);
        d.strideInBytes = 0;
        d.initialData = src.meshletTriangles;
        d.initialDataSize = src.meshletTriangleByteCount;
        d.name = L"Primitive/meshletTriangles";
        prim.mlTris = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);

        d.viewKind = BufferCreateDesc::ViewKind::Structured;
        d.sizeInBytes = src.meshletCount * sizeof(MeshletBounds);
        d.strideInBytes = sizeof(MeshletBounds);
        d.initialData = src.meshletBounds;
        d.initialDataSize = d.sizeInBytes;
        d.name = L"Primitive/meshletBounds";
        prim.mlBounds = m_RenderDevice.CreateBuffer(m_BindlessHeaps, cmd.Get(), d);

        m_Primitives.push_back(std::move(prim));
    }
}

void SceneResources::ImportSceneInstances(const LoadedScene& scene)
{
    m_Instances = scene.instances;
    m_InstancesDirty = true;
}
