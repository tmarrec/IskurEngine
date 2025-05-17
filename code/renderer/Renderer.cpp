// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Renderer.h"

#include <D3D12MemAlloc.h>
#include <algorithm>
#include <chrono>
#include <d3dx12.h>
#include <execution>

#include "../common/Asserts.h"
#include "../common/CommandLineArguments.h"
#include "../window/Window.h"
#include "Camera.h"
#include "CompileShader.h"
#include "Constants.h"

namespace
{
D3D12_TEXTURE_ADDRESS_MODE MapAddressMode(i32 gltfAddressMode)
{
    switch (gltfAddressMode)
    {
    case TINYGLTF_TEXTURE_WRAP_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_FILTER MapFilterMode(i32 magFilter, i32 minFilter)
{
    if (magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
    {
        if (minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST || minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST)
        {
            return D3D12_FILTER_MIN_MAG_MIP_POINT;
        }
        if (minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR || minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)
        {
            return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
        }
    }
    else if (magFilter == TINYGLTF_TEXTURE_FILTER_LINEAR)
    {
        if (minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST || minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST)
        {
            return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
        }
        if (minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR || minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)
        {
            return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

DXGI_FORMAT GetDXGIFormat(const tinygltf::Image& image)
{
    const i32 comp = image.component;
    const i32 bits = image.bits;
    const i32 ptype = image.pixel_type;

    if (ptype == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && bits == 8)
    {
        switch (comp)
        {
        case 1:
            return DXGI_FORMAT_R8_UNORM;
        case 2:
            return DXGI_FORMAT_R8G8_UNORM;
        case 3: // no RGB8 expand X
        case 4:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
    if (ptype == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && bits == 16)
    {
        switch (comp)
        {
        case 1:
            return DXGI_FORMAT_R16_UNORM;
        case 2:
            return DXGI_FORMAT_R16G16_UNORM;
        case 3: // no RGB16 expand X
        case 4:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        }
    }

    if (ptype == TINYGLTF_COMPONENT_TYPE_FLOAT && bits == 32)
    {
        switch (comp)
        {
        case 1:
            return DXGI_FORMAT_R32_FLOAT;
        case 2:
            return DXGI_FORMAT_R32G32_FLOAT;
        case 3: // no RGB32F expand X
        case 4:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    }

    IE_Assert(false); // not implemented!
    return DXGI_FORMAT_UNKNOWN;
}
} // namespace

void Renderer::Init()
{
    const Window& window = Window::GetInstance();

    u32 dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_Debug))))
    {
        m_Debug->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    IE_Check(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_DxgiFactory)));

    static constexpr D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;
    i32 currentAdapterIndex = 0;
    bool adapterFound = false;
    while (m_DxgiFactory->EnumAdapters1(currentAdapterIndex, &m_Adapter) != DXGI_ERROR_NOT_FOUND) // Find a GPU that supports DX12
    {
        DXGI_ADAPTER_DESC1 desc;
        IE_Check(m_Adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) // We don't want software GPU
        {
            currentAdapterIndex++;
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(m_Adapter.Get(), featureLevel, __uuidof(ID3D12Device), nullptr)))
        {
            adapterFound = true;
            break;
        }
        m_Adapter->Release();
        currentAdapterIndex++;
    }
    IE_Assert(adapterFound);
    IE_Check(D3D12CreateDevice(m_Adapter.Get(), featureLevel, IID_PPV_ARGS(&m_Device)));

    // >>> Create allocator
    const D3D12MA::ALLOCATOR_DESC allocatorDesc = {
        .Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED | D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED,
        .pDevice = m_Device.Get(),
        .PreferredBlockSize = 0,
        .pAllocationCallbacks = nullptr,
        .pAdapter = m_Adapter.Get(),
    };
    IE_Check(CreateAllocator(&allocatorDesc, &m_Allocator));
    // <<< Create allocator

    // >>> Create command queue
    constexpr D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    IE_Check(m_Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_CommandQueue)));
    // <<< Create command queue

    // >>> Create swapchain
    const uint2& windowRes = window.GetResolution();
    const DXGI_SWAP_CHAIN_DESC1 swapchainDesc = {
        .Width = windowRes.x,
        .Height = windowRes.y,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Stereo = false,
        .SampleDesc =
            {
                .Count = 1,
                .Quality = 0,
            },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = IE_Constants::frameInFlightCount,
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
        .Flags = 0,
    };

    IDXGISwapChain1* tempSwapchain = nullptr;
    IE_Check(m_DxgiFactory->CreateSwapChainForHwnd(m_CommandQueue.Get(), window.GetHwnd(), &swapchainDesc, nullptr, nullptr, &tempSwapchain));
    IE_Check(m_DxgiFactory->MakeWindowAssociation(window.GetHwnd(), DXGI_MWA_NO_ALT_ENTER)); // No fullscreen with alt enter
    IE_Check(tempSwapchain->QueryInterface(IID_PPV_ARGS(&m_Swapchain)));
    // <<< Create swapchain

    // >>> Create fences and command allocators
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_AllFrameData[i].renderFinishedFence)));

        for (u32 j = 0; j < AlphaMode_Count; ++j)
        {
            IE_Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_AllFrameData[i].commandAllocators[j])));
        }
        IE_Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_AllFrameData[i].rtCommandAllocator)));
        IE_Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_AllFrameData[i].depthCommandAllocator)));
    }
    // <<< Create fences and command allocators

    // >>> Create color render target
    m_ColorTarget = IE_MakeSharedPtr<RenderTarget>();
    constexpr D3D12_DESCRIPTOR_HEAP_DESC descriptorRtvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = IE_Constants::frameInFlightCount,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorRtvHeapDesc, IID_PPV_ARGS(&m_ColorTarget->heap)));
    IE_Check(m_ColorTarget->heap->SetName(L"Color : Heap"));
    constexpr D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_ColorTarget->heap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        ID3D12Resource* renderTarget;
        IE_Check(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&renderTarget)));
        m_Device->CreateRenderTargetView(renderTarget, &rtvDesc, rtvHandle);
        rtvHandle.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); // Offset
        IE_Check(renderTarget->SetName(L"Color : RTV"));
        m_ColorTarget->resource.Add(renderTarget);
    }
    // <<< Create color render target

    // >>> Create depth stencil target
    m_DepthStencil = IE_MakeSharedPtr<RenderTarget>();
    constexpr D3D12_DESCRIPTOR_HEAP_DESC descriptorDsvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorDsvHeapDesc, IID_PPV_ARGS(&m_DepthStencil->heap)));
    IE_Check(m_DepthStencil->heap->SetName(L"Depth/Stencil : Heap"));
    constexpr D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
        .Flags = D3D12_DSV_FLAG_NONE,
    };
    constexpr D3D12_CLEAR_VALUE depthOptimizedClearValue = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .DepthStencil =
            {
                .Depth = 1.0f,
                .Stencil = 0,
            },
    };
    const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, windowRes.x, windowRes.y, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    constexpr D3D12MA::ALLOCATION_DESC allocDesc = {
        .HeapType = D3D12_HEAP_TYPE_DEFAULT,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_DepthStencil->heap->GetCPUDescriptorHandleForHeapStart();
    ID3D12Resource* depthStencil;
    D3D12MA::Allocation* allocation;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue, &allocation, IID_PPV_ARGS(&depthStencil)));
    m_Device->CreateDepthStencilView(depthStencil, &dsvDesc, dsvHandle);
    dsvHandle.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); // Offset
    IE_Check(depthStencil->SetName(L"Depth/Stencil : DSV"));
    m_DepthStencil->resource.Add(depthStencil);
    // <<< Create depth stencil target

    // >>> Create main descriptor heap (CBV_SRV_UAV) for bindless
    constexpr D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 65536,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_CBVSRVUAVHeap)));
    // <<< Create main descriptor heap (CBV_SRV_UAV) for bindless

    // >>> Create main descriptor heap (SAMPLER) for bindless
    constexpr D3D12_DESCRIPTOR_HEAP_DESC samplerDescriptorHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = 4080,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&samplerDescriptorHeapDesc, IID_PPV_ARGS(&m_SamplerHeap)));
    // <<< Create main descriptor heap (SAMPLER) for bindless

    AllocateUploadBuffer(nullptr, sizeof(GlobalConstants) * IE_Constants::frameInFlightCount, 0, m_ConstantsBuffer, L"Color Constants");

    m_AmplificationShader = LoadShader(IE_SHADER_TYPE_AMPLIFICATION, L"asBasic.hlsl");
    m_MeshShader = LoadShader(IE_SHADER_TYPE_MESH, L"msBasic.hlsl");
    m_PixelShader = LoadShader(IE_SHADER_TYPE_PIXEL, L"psBasic.hlsl");

    const Vector renderTargets = {m_ColorTarget};
    m_PipelineStates = CreatePipelineState(m_AmplificationShader, m_MeshShader, m_PixelShader, renderTargets, m_DepthStencil);

    m_Viewport = {
        .TopLeftX = 0.f,
        .TopLeftY = 0.f,
        .Width = static_cast<f32>(window.GetResolution().x),
        .Height = static_cast<f32>(window.GetResolution().y),
        .MinDepth = 0.f,
        .MaxDepth = 1.f,
    };
    m_Rect = {
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(window.GetResolution().x),
        .bottom = static_cast<i32>(window.GetResolution().y),
    };

    GetOrWaitNextFrame();

    // Create command list for setup
    ComPtr<ID3D12GraphicsCommandList7> cmdList;
    IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Pfd->commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)));

    const CommandLineArguments& args = GetCommandLineArguments();
    String sceneFile = String("data/scenes/");
    String fileName;
    if (!args.sceneFile.Empty())
    {
        fileName = args.sceneFile;

        const u32 lastCharIdx = fileName.Size() - 1;
        if (fileName[lastCharIdx - 3] != '.' || fileName[lastCharIdx - 2] != 'g' || fileName[lastCharIdx - 1] != 'l' || fileName[lastCharIdx] != 'b')
        {
            fileName += ".glb";
        }
    }
    else
    {
        fileName = "chess.glb";
    }
    sceneFile += fileName;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;
    IE_Assert(loader.LoadBinaryFromFile(&model, &err, &warn, sceneFile.Data()));
    IE_Assert(warn.empty());
    IE_Assert(err.empty());
    m_World = IE_MakeSharedPtr<World>(model);

    // >> Store all textures
    // Because the textures are the first thing being uploaded to the heap, we can keep their gltf indices!
    for (const tinygltf::Texture& texture : model.textures)
    {
        const tinygltf::Image& image = model.images[texture.source];
        const DXGI_FORMAT format = GetDXGIFormat(image);
        UploadTexture(cmdList, image.width, image.height, image.image.data(), static_cast<u32>(image.image.size()) * sizeof(u8), format);
    }
    // << Store all textures

    // >>> Store all samplers
    for (const tinygltf::Sampler& sampler : model.samplers)
    {
        const D3D12_SAMPLER_DESC samplerDesc = {
            .Filter = MapFilterMode(sampler.magFilter, sampler.minFilter),
            .AddressU = MapAddressMode(sampler.wrapS),
            .AddressV = MapAddressMode(sampler.wrapT),
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NONE,
            .MinLOD = 0.0f,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += m_SamplerHeapLastIdx * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        m_Device->CreateSampler(&samplerDesc, srvHandle);
        m_SamplerHeapLastIdx++;
    }
    // <<< Store all samplers

    // >>> Store all materials
    for (const tinygltf::Material& material : model.materials)
    {
        Material m;

        const tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;
        if (!pbr.baseColorFactor.empty())
        {
            m.baseColorFactor = {static_cast<f32>(pbr.baseColorFactor[0]), static_cast<f32>(pbr.baseColorFactor[1]), static_cast<f32>(pbr.baseColorFactor[2]),
                                 static_cast<f32>(pbr.baseColorFactor[3])};
        }
        else
        {
            m.baseColorFactor = {0, 0, 0, 0};
        }
        m.metallicFactor = static_cast<f32>(pbr.metallicFactor);
        m.roughnessFactor = static_cast<f32>(pbr.roughnessFactor);

        m.baseColorTextureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
        {
            m.baseColorSamplerIndex = model.textures[material.pbrMetallicRoughness.baseColorTexture.index].sampler;
        }
        else
        {
            m.baseColorSamplerIndex = -1;
        }

        m.metallicRoughnessTextureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
        {
            m.metallicRoughnessSamplerIndex = model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].sampler;
        }
        else
        {
            m.metallicRoughnessSamplerIndex = -1;
        }

        m.normalTextureIndex = material.normalTexture.index;
        m.normalScale = static_cast<float>(material.normalTexture.scale);

        if (material.normalTexture.index != -1)
        {
            m.normalSamplerIndex = model.textures[material.normalTexture.index].sampler;
        }
        else
        {
            m.normalSamplerIndex = -1;
        }

        m.alphaMode = AlphaMode_Opaque;
        if (material.alphaMode == "BLEND")
        {
            m.alphaMode = AlphaMode_Blend;
        }
        if (material.alphaMode == "MASK")
        {
            m.alphaMode = AlphaMode_Mask;
        }
        m.alphaCutoff = static_cast<float>(material.alphaCutoff);

        m_Materials.Add(m);
    }

    if (!m_Materials.Empty())
    {
        m_MaterialsBuffer = CreateStructuredBuffer(m_Materials.ByteSize(), sizeof(Material), L"Materials");
        SetBufferData(cmdList, m_MaterialsBuffer, m_Materials.Data(), m_Materials.ByteSize(), 0);
    }
    // <<< Store all materials

    // >>> Upload primitives to GPU
    const Vector<SharedPtr<Primitive>>& primitives = m_World->GetPrimitives();
    Vector<u32> indices(primitives.Size());
    for (u32 i = 0; i < indices.Size(); ++i)
    {
        indices[i] = i;
    }
    std::for_each(std::execution::par_unseq, indices.begin(), indices.end(), [&primitives, &model](u32 i) { primitives[i]->Process(model); });

    for (const SharedPtr<Primitive>& primitive : primitives)
    {
        primitive->m_VerticesBuffer = CreateStructuredBuffer(primitive->m_Vertices.ByteSize(), sizeof(Vertex), L"Vertices");
        primitive->m_MeshletsBuffer = CreateStructuredBuffer(primitive->m_Meshlets.ByteSize(), sizeof(meshopt_Meshlet), L"Meshlets");
        primitive->m_MeshletVerticesBuffer = CreateStructuredBuffer(primitive->m_MeshletVertices.ByteSize(), sizeof(u32), L"Meshlet Vertices");
        primitive->m_MeshletTrianglesBuffer = CreateBytesBuffer(primitive->m_MeshletTriangles.Size(), L"Meshlet Triangles");
        primitive->m_MeshletBoundsBuffer = CreateStructuredBuffer(primitive->m_MeshletBounds.ByteSize(), sizeof(meshopt_Bounds), L"Meshlet Bounds");

        SetBufferData(cmdList, primitive->m_VerticesBuffer, primitive->m_Vertices.Data(), primitive->m_Vertices.ByteSize(), 0);
        SetBufferData(cmdList, primitive->m_MeshletsBuffer, primitive->m_Meshlets.Data(), primitive->m_Meshlets.ByteSize(), 0);
        SetBufferData(cmdList, primitive->m_MeshletVerticesBuffer, primitive->m_MeshletVertices.Data(), primitive->m_MeshletVertices.ByteSize(), 0);
        SetBufferData(cmdList, primitive->m_MeshletTrianglesBuffer, primitive->m_MeshletTriangles.Data(), primitive->m_MeshletTriangles.ByteSize(), 0);
        SetBufferData(cmdList, primitive->m_MeshletBoundsBuffer, primitive->m_MeshletBounds.Data(), primitive->m_MeshletBounds.ByteSize(), 0);
    }

    for (const SharedPtr<Primitive>& primitive : primitives)
    {
        m_PrimitivesPerAlphaMode[m_Materials[primitive->m_MaterialIdx].alphaMode].Add(primitive);
        m_Primitives.Add(primitive);
    }
    // <<< Upload primitives to GPU

    // >>> Create Depth SRV and Sampler
    ID3D12Resource* depthTex = m_DepthStencil->resource[0];
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvHandle = GetNewCBVSRVUAVHeapHandle();
    constexpr D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = DXGI_FORMAT_R32_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D =
            {
                .MipLevels = 1,
            },
    };
    m_Device->CreateShaderResourceView(depthTex, &srvDesc, depthSrvHandle);
    m_DepthResourceSRVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CBVSRVUAVHeap->GetGPUDescriptorHandleForHeapStart(), m_CBVSRVUAVHeapLastIdx - 1,
                                                                    m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    constexpr D3D12_DESCRIPTOR_HEAP_DESC sampHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    IE_Check(m_Device->CreateDescriptorHeap(&sampHeapDesc, IID_PPV_ARGS(&m_SamplerDepthRTHeap)));

    constexpr D3D12_SAMPLER_DESC linearClampDesc = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MipLODBias = 0,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
        .MinLOD = 0,
        .MaxLOD = D3D12_FLOAT32_MAX,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE sampHandle = m_SamplerDepthRTHeap->GetCPUDescriptorHandleForHeapStart();
    m_Device->CreateSampler(&linearClampDesc, sampHandle);
    // <<< Create Depth SRV and Sampler

    // >>> Setup Raytracing
    if (IE_Constants::enableRaytracedShadows)
    {
        SetupRaytracing(cmdList);
    }
    // <<< Setup Raytracing

    IE_Check(cmdList->Close());
    ID3D12CommandList* pCmdList = cmdList.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmdList);
    IE_Check(m_CommandQueue->Signal(m_Pfd->renderFinishedFence.Get(), m_Pfd->renderFinishedFenceValue));
    cmdList.Reset();
}

void Renderer::Render()
{
    Camera& camera = Camera::GetInstance();

    /*
    const std::chrono::microseconds ms = duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    );
    camera.m_SunDir.x = static_cast<f32>(sin(static_cast<double>(ms.count()) / 25000000.0));
    camera.m_SunDir.z = static_cast<f32>(-sin(static_cast<double>(ms.count()) / 20000000.0));
    IE_Log("{} {}", camera.m_SunDir.x, camera.m_SunDir.z);
    */

    camera.m_SunDir.x = 0.04225999f;
    camera.m_SunDir.z = -0.66877323f;

    GetOrWaitNextFrame();

    const Array<ID3D12DescriptorHeap*, 2> descriptorHeaps = {m_CBVSRVUAVHeap.Get(), m_SamplerHeap.Get()};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_ColorTarget->heap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_FrameInFlightIdx * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_DepthStencil->heap->GetCPUDescriptorHandleForHeapStart();
    dsvHandle.ptr += 0 * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // Constant Buffer
    const float4x4 view = camera.GetViewMatrix();
    const float4x4 proj = camera.GetProjection();
    const float4x4 projCull = camera.GetProjection();
    const float4x4 vp = (view * projCull).Transposed();
    m_Constants.view = view.Transposed();
    m_Constants.proj = proj.Transposed();
    m_Constants.planes[0] = (vp[3] + vp[0]).Normalized();
    m_Constants.planes[1] = (vp[3] - vp[0]).Normalized();
    m_Constants.planes[2] = (vp[3] + vp[1]).Normalized();
    m_Constants.planes[3] = (vp[3] - vp[1]).Normalized();
    m_Constants.planes[4] = vp[2].Normalized();
    m_Constants.planes[5] = (vp[3] - vp[2]).Normalized();
    m_Constants.cameraPos = camera.GetPosition();
    m_Constants.sunDir = camera.m_SunDir;
    m_Constants.raytracingOutputIndex = m_RaytracingOutputIndex;
    SetResourceBufferData(m_ConstantsBuffer, &m_Constants, sizeof(GlobalConstants), m_FrameInFlightIdx * sizeof(GlobalConstants));

    Array<u32, AlphaMode_Count> alphaModeIndices;
    for (u32 i = 0; i < alphaModeIndices.Size(); ++i)
    {
        alphaModeIndices[i] = i;
    }

    // Depth Pre-Pass
    {
        IE_Check(m_Pfd->depthCommandAllocator->Reset());

        ComPtr<ID3D12GraphicsCommandList7> depthCmd;
        IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Pfd->depthCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&depthCmd)));

        depthCmd->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        depthCmd->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
        depthCmd->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
        depthCmd->SetGraphicsRootSignature(m_PipelineStates->rootSignatures[AlphaMode_Opaque].Get());
        depthCmd->SetPipelineState(m_PipelineStates->depthPipelineState.Get());
        depthCmd->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(GlobalConstants));
        depthCmd->RSSetViewports(1, &m_Viewport);
        depthCmd->RSSetScissorRects(1, &m_Rect);

        for (const Vector<SharedPtr<Primitive>>& primitives : m_PrimitivesPerAlphaMode)
        {
            for (const SharedPtr<Primitive>& primitive : primitives)
            {
                PrimitiveRootConstants rootConstants = {.world = primitive->GetTransform().Transposed(),
                                                        .meshletCount = primitive->m_Meshlets.Size(),
                                                        .materialIdx = primitive->m_MaterialIdx,
                                                        .verticesBufferIndex = primitive->m_VerticesBuffer->index,
                                                        .meshletsBufferIndex = primitive->m_MeshletsBuffer->index,
                                                        .meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->index,
                                                        .meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->index,
                                                        .meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->index,
                                                        .materialsBufferIndex = m_MaterialsBuffer->index};
                depthCmd->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
                depthCmd->DispatchMesh((primitive->m_Meshlets.Size() + 31) / 32, 1, 1);
            }
        }

        Barrier(depthCmd, m_DepthStencil->resource[0], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);

        IE_Check(depthCmd->Close());
        ID3D12CommandList* lists[] = {depthCmd.Get()};
        m_CommandQueue->ExecuteCommandLists(1, lists);
    }

    static constexpr u32 raytraceCmdListCount = IE_Constants::enableRaytracedShadows ? 1 : 0;
    Array<ID3D12GraphicsCommandList7*, raytraceCmdListCount + AlphaMode_Count> cmdLists;

    // RayTracing
    if (IE_Constants::enableRaytracedShadows)
    {
        IE_Check(m_Pfd->rtCommandAllocator->Reset());

        ID3D12GraphicsCommandList7*& rtcmdList = cmdLists[0];

        IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Pfd->rtCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&rtcmdList)));
        u32 swapchainWidth;
        u32 swapchainHeight;
        IE_Check(m_Swapchain->GetSourceSize(&swapchainWidth, &swapchainHeight));

        rtcmdList->SetComputeRootSignature(m_RaytracingGlobalRootSignature.Get());
        rtcmdList->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());

        rtcmdList->SetPipelineState1(m_DxrStateObject.Get());

        rtcmdList->SetComputeRootDescriptorTable(RtRootSignatureParams::OutputViewSlot, m_RaytracingOutputResourceUAVGpuDescriptor);
        rtcmdList->SetComputeRootShaderResourceView(RtRootSignatureParams::AccelerationStructureSlot, m_TLAS->GetGPUVirtualAddress());
        rtcmdList->SetComputeRootDescriptorTable(RtRootSignatureParams::DepthBufferSlot, m_DepthResourceSRVGpuDescriptor);
        rtcmdList->SetComputeRootDescriptorTable(RtRootSignatureParams::SamplerSlot, m_SamplerDepthRTHeap->GetGPUDescriptorHandleForHeapStart());

        const float2 cameraZNearFar = camera.GetZNearFar();
        const float3 cameraPos = camera.GetPosition();
        const RtRootConstants rayTraceRootConstants = {
            .invViewProj = (view * proj).Inversed(),
            .zNear = cameraZNearFar.x,
            .zFar = cameraZNearFar.y,
            .resolutionType = static_cast<u32>(IE_Constants::raytracedShadowsType), // I should use defines to DXC when compiling but apparently there
                                                                                    // is an DXC issue with -D defines with raytracing shaders...
            .sunDir = camera.m_SunDir.Normalized(),
            .frameIndex = m_FrameIndex,
            .cameraPos = cameraPos,
        };
        rtcmdList->SetComputeRoot32BitConstants(RtRootSignatureParams::ConstantsSlot, sizeof(RtRootConstants) / sizeof(u32), &rayTraceRootConstants, 0);

        uint2 rtShadowsRes = {swapchainWidth, swapchainHeight};
        switch (IE_Constants::raytracedShadowsType)
        {
        case RayTracingResolution::Full:
            break;
        case RayTracingResolution::FullX_HalfY:
            rtShadowsRes.y /= 2;
            break;
        case RayTracingResolution::Half:
            rtShadowsRes /= 2;
            break;
        case RayTracingResolution::Quarter:
            rtShadowsRes /= 4;
            break;
        }

        const D3D12_DISPATCH_RAYS_DESC dispatchDesc = {
            .RayGenerationShaderRecord =
                {
                    .StartAddress = m_RayGenShaderTable->GetGPUVirtualAddress(),
                    .SizeInBytes = m_RayGenShaderTable->GetDesc().Width,
                },
            .MissShaderTable =
                {
                    .StartAddress = m_MissShaderTable->GetGPUVirtualAddress(),
                    .SizeInBytes = m_MissShaderTable->GetDesc().Width,
                    .StrideInBytes = m_MissShaderTable->GetDesc().Width,
                },
            .HitGroupTable =
                {
                    .StartAddress = m_HitGroupShaderTable->GetGPUVirtualAddress(),
                    .SizeInBytes = m_HitGroupShaderTable->GetDesc().Width,
                    .StrideInBytes = m_HitGroupShaderTable->GetDesc().Width,
                },
            .Width = rtShadowsRes.x,
            .Height = rtShadowsRes.y,
            .Depth = 1,
        };
        rtcmdList->DispatchRays(&dispatchDesc);

        IE_Check(rtcmdList->Close());
    }

    std::for_each(std::execution::par_unseq, alphaModeIndices.begin(), alphaModeIndices.end(), [&](u32 i) {
        const AlphaMode alphaMode = static_cast<AlphaMode>(i);

        IE_Check(m_Pfd->commandAllocators[i]->Reset());
        ID3D12GraphicsCommandList7*& cmdList = cmdLists[raytraceCmdListCount + i];
        IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Pfd->commandAllocators[i].Get(), nullptr, IID_PPV_ARGS(&cmdList)));

        Barrier(cmdList, m_ColorTarget->resource[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(cmdList, m_DepthStencil->resource[0], D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        cmdList->SetDescriptorHeaps(descriptorHeaps.Size(), descriptorHeaps.Data());
        cmdList->SetGraphicsRootSignature(m_PipelineStates->rootSignatures[alphaMode].Get());
        cmdList->RSSetViewports(1, &m_Viewport);
        cmdList->RSSetScissorRects(1, &m_Rect);

        if (alphaMode == AlphaMode_Opaque)
        {
            const Array<f32, 4> color = {0, 0, 0, 1};
            cmdList->ClearRenderTargetView(rtvHandle, color.Data(), 0, nullptr);
        }

        cmdList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);
        cmdList->SetPipelineState(m_PipelineStates->pipelineStates[alphaMode].Get());
        cmdList->SetGraphicsRootConstantBufferView(1, m_ConstantsBuffer->GetGPUVirtualAddress() + m_FrameInFlightIdx * sizeof(GlobalConstants));

        for (const SharedPtr<Primitive>& primitive : m_PrimitivesPerAlphaMode[alphaMode])
        {
            const PrimitiveRootConstants rootConstants = {
                .world = primitive->GetTransform().Transposed(),
                .meshletCount = primitive->m_Meshlets.Size(),
                .materialIdx = primitive->m_MaterialIdx,
                .verticesBufferIndex = primitive->m_VerticesBuffer->index,
                .meshletsBufferIndex = primitive->m_MeshletsBuffer->index,
                .meshletVerticesBufferIndex = primitive->m_MeshletVerticesBuffer->index,
                .meshletTrianglesBufferIndex = primitive->m_MeshletTrianglesBuffer->index,
                .meshletBoundsBufferIndex = primitive->m_MeshletBoundsBuffer->index,
                .materialsBufferIndex = m_MaterialsBuffer->index,
            };
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(PrimitiveRootConstants) / 4, &rootConstants, 0);
            cmdList->DispatchMesh((primitive->m_Meshlets.Size() + 32 - 1) / 32, 1, 1);
        }

        Barrier(cmdList, m_ColorTarget->resource[m_FrameInFlightIdx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        IE_Check(cmdList->Close());
    });

    m_CommandQueue->ExecuteCommandLists(cmdLists.Size(), reinterpret_cast<ID3D12CommandList* const*>(cmdLists.Data()));

    IE_Check(m_CommandQueue->Signal(m_Pfd->renderFinishedFence.Get(), m_Pfd->renderFinishedFenceValue));

    IE_Check(m_Swapchain->Present(0, 0));

    m_FrameIndex++;
}

void Renderer::SetupRaytracing(ComPtr<ID3D12GraphicsCommandList7>& cmdList)
{
    // Create 2D output texture for raytracing.
    u32 swapchainWidth;
    u32 swapchainHeight;
    m_Swapchain->GetSourceSize(&swapchainWidth, &swapchainHeight);

    // Create the output resource
    {
        CD3DX12_RESOURCE_DESC uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, swapchainWidth, swapchainHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CD3DX12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
        UAVDesc.Format = DXGI_FORMAT_R16_FLOAT;
        UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        IE_Check(m_Device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_RaytracingOutput)));

        D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle = GetNewCBVSRVUAVHeapHandle();
        m_RaytracingOutputIndex = m_CBVSRVUAVHeapLastIdx - 1;

        m_Device->CreateUnorderedAccessView(m_RaytracingOutput.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
        m_RaytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CBVSRVUAVHeap->GetGPUDescriptorHandleForHeapStart(), m_CBVSRVUAVHeapLastIdx - 1,
                                                                                   m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        m_RaytracingOutput->SetName(L"Raytracing Output");
    }

    const CD3DX12_DESCRIPTOR_RANGE1 depthRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    const CD3DX12_DESCRIPTOR_RANGE1 samplerRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);

    const CD3DX12_DESCRIPTOR_RANGE1 UAVDescriptor(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    Array<CD3DX12_ROOT_PARAMETER1, RtRootSignatureParams::Count> rootParameters;
    rootParameters[RtRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &UAVDescriptor);
    rootParameters[RtRootSignatureParams::AccelerationStructureSlot].InitAsShaderResourceView(0);
    rootParameters[RtRootSignatureParams::ConstantsSlot].InitAsConstants(sizeof(RtRootConstants) / sizeof(u32), 1);
    rootParameters[RtRootSignatureParams::DepthBufferSlot].InitAsDescriptorTable(1, &depthRange); // bind depth SRV at t1
    rootParameters[RtRootSignatureParams::SamplerSlot].InitAsDescriptorTable(1, &samplerRange);   // bind depth SRV at t1

    const CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSignatureDesc(rootParameters.Size(), rootParameters.Data());

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    IE_Check(D3D12SerializeVersionedRootSignature(&globalRootSignatureDesc, &blob, &error));
    IE_Check(m_Device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RaytracingGlobalRootSignature)));

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

    const SharedPtr<Shader> raytracingShader = LoadShader(IE_SHADER_TYPE_LIB, L"rtShadows.hlsl");

    const D3D12_SHADER_BYTECODE libdxil{raytracingShader->bytecodes[AlphaMode_Opaque].pShaderBytecode, raytracingShader->bytecodes[AlphaMode_Opaque].BytecodeLength};
    lib->SetDXILLibrary(&libdxil);
    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"Miss");

    CD3DX12_HIT_GROUP_SUBOBJECT* hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(L"ClosestHit");
    hitGroup->SetHitGroupExport(L"HitGroup");
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* shaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    u32 payloadSize = 1 * sizeof(u32);   // struct RayPayload is just one uint
    u32 attributeSize = 2 * sizeof(f32); // BuiltInTriangleIntersectionAttributes
    shaderConfig->Config(payloadSize, attributeSize);

    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_RaytracingGlobalRootSignature.Get());

    CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    static constexpr u32 maxRecursionDepth = 1;
    pipelineConfig->Config(maxRecursionDepth);

    // Create the state object.
    IE_Check(m_Device->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_DxrStateObject)));

    Vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    for (const SharedPtr<Primitive>& prim : m_Primitives)
    {
        AllocateUploadBuffer(prim->m_Vertices.Data(), prim->m_Vertices.ByteSize(), 0, prim->m_VertexBuffer, L"Primitive/VertexBuffer");
        AllocateUploadBuffer(prim->m_Indices.Data(), prim->m_Indices.ByteSize(), 0, prim->m_IndexBuffer, L"Primitive/IndexBuffer");

        const D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {
            .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
            .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
            .Triangles =
                {
                    .Transform3x4 = 0,
                    .IndexFormat = DXGI_FORMAT_R32_UINT,
                    .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
                    .IndexCount = prim->m_Indices.Size(),
                    .VertexCount = prim->m_Vertices.Size(),
                    .IndexBuffer = prim->m_IndexBuffer->GetGPUVirtualAddress(),
                    .VertexBuffer =
                        {
                            .StartAddress = prim->m_VertexBuffer->GetGPUVirtualAddress(),
                            .StrideInBytes = sizeof(Vertex),
                        },
                },
        };

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuildInfo;
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
            .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
            .NumDescs = 1,
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .pGeometryDescs = &geometryDesc,
        };
        m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuildInfo);
        IE_Assert(blasPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        AllocateUAVBuffer(static_cast<u32>(blasPrebuildInfo.ResultDataMaxSizeInBytes), prim->m_BLAS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS");
        AllocateUAVBuffer(static_cast<u32>(blasPrebuildInfo.ScratchDataSizeInBytes), prim->m_ScratchResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");

        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuildDesc = {
            .DestAccelerationStructureData = prim->m_BLAS->GetGPUVirtualAddress(),
            .Inputs = blasInputs,
            .ScratchAccelerationStructureData = prim->m_ScratchResource->GetGPUVirtualAddress(),
        };
        cmdList->BuildRaytracingAccelerationStructure(&blasBuildDesc, 0, nullptr);

        const float4x4 transform = prim->GetTransform();
        const D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {.Transform = {{transform[0][0], transform[1][0], transform[2][0], transform[3][0]},
                                                                           {transform[0][1], transform[1][1], transform[2][1], transform[3][1]},
                                                                           {transform[0][2], transform[1][2], transform[2][2], transform[3][2]}},
                                                             .InstanceMask = 1,
                                                             .AccelerationStructure = prim->m_BLAS->GetGPUVirtualAddress()};
        instanceDescs.Add(instanceDesc);
    }

    for (const SharedPtr<Primitive>& prim : m_Primitives)
    {
        UAVBarrier(cmdList, prim->m_BLAS);
    }

    AllocateUploadBuffer(instanceDescs.Data(), instanceDescs.ByteSize(), 0, m_InstanceDescs, L"InstanceDescs");

    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = instanceDescs.Size(),
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = m_InstanceDescs->GetGPUVirtualAddress(),
    };
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo;
    m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
    IE_Assert(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    AllocateUAVBuffer(static_cast<u32>(topLevelPrebuildInfo.ScratchDataSizeInBytes), m_ScratchResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");
    AllocateUAVBuffer(static_cast<u32>(topLevelPrebuildInfo.ResultDataMaxSizeInBytes), m_TLAS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS");

    // Top Level Acceleration Structure desc
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {
        .DestAccelerationStructureData = m_TLAS->GetGPUVirtualAddress(),
        .Inputs = topLevelInputs,
        .ScratchAccelerationStructureData = m_ScratchResource->GetGPUVirtualAddress(),
    };
    cmdList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);

    ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    IE_Check(m_DxrStateObject.As(&stateObjectProperties));

    static constexpr u32 shaderRecordSize = (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1)) & ~(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1);
    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(shaderRecordSize);
    const CD3DX12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Ray gen shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_RayGenShaderTable)));
    m_RayGenShaderTable->SetName(L"RayGenShaderTable");
    SetResourceBufferData(m_RayGenShaderTable, stateObjectProperties->GetShaderIdentifier(L"Raygen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);

    // Miss shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_MissShaderTable)));
    m_MissShaderTable->SetName(L"MissShaderTable");
    SetResourceBufferData(m_MissShaderTable, stateObjectProperties->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);

    // Hit group shader table
    IE_Check(m_Device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_HitGroupShaderTable)));
    m_HitGroupShaderTable->SetName(L"HitGroupShaderTable");
    SetResourceBufferData(m_HitGroupShaderTable, stateObjectProperties->GetShaderIdentifier(L"HitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 0);
}

SharedPtr<Buffer> Renderer::CreateStructuredBuffer(u32 sizeInBytes, u32 strideInBytes, const WString& name, D3D12_HEAP_TYPE heapType)
{
    const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    const D3D12MA::ALLOCATION_DESC allocDesc = {
        .HeapType = heapType,
    };
    ID3D12Resource* dxBuffer;
    D3D12MA::Allocation* allocation;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, &allocation, IID_PPV_ARGS(&dxBuffer)));
    IE_Check(dxBuffer->SetName(name.Data()));

    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer =
            {
                .FirstElement = 0,
                .NumElements = sizeInBytes / strideInBytes,
                .StructureByteStride = strideInBytes,
                .Flags = D3D12_BUFFER_SRV_FLAG_NONE,
            },
    };
    const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = GetNewCBVSRVUAVHeapHandle();
    m_Device->CreateShaderResourceView(dxBuffer, &srvDesc, srvHandle);

    const Buffer buffer = {
        .buffer = dxBuffer,
        .srvHandle = srvHandle,
        .index = m_CBVSRVUAVHeapLastIdx - 1,
    };
    return IE_MakeSharedPtr<Buffer>(buffer);
}

SharedPtr<Buffer> Renderer::CreateBytesBuffer(u32 numElements, const WString& name, D3D12_HEAP_TYPE heapType)
{
    const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(numElements);
    const D3D12MA::ALLOCATION_DESC allocDesc = {
        .HeapType = heapType,
    };
    ID3D12Resource* dxBuffer;
    D3D12MA::Allocation* allocation;
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, &allocation, IID_PPV_ARGS(&dxBuffer)));
    IE_Check(dxBuffer->SetName(name.Data()));

    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer =
            {
                .FirstElement = 0,
                .NumElements = numElements / 4,
                .StructureByteStride = 0,
                .Flags = D3D12_BUFFER_SRV_FLAG_RAW,
            },
    };
    const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = GetNewCBVSRVUAVHeapHandle();
    m_Device->CreateShaderResourceView(dxBuffer, &srvDesc, srvHandle);

    const Buffer buffer = {
        .buffer = dxBuffer,
        .srvHandle = srvHandle,
        .index = m_CBVSRVUAVHeapLastIdx - 1,
    };
    return IE_MakeSharedPtr<Buffer>(buffer);
}

SharedPtr<PipelineState> Renderer::CreatePipelineState(const SharedPtr<Shader>& amplificationShader, const SharedPtr<Shader>& meshShader, const SharedPtr<Shader>& pixelShader,
                                                       Vector<SharedPtr<RenderTarget>> renderTargets, const SharedPtr<RenderTarget>& depthStencil) const
{
    PipelineState pipelineState;

    for (AlphaMode alphaMode = AlphaMode_Opaque; alphaMode < AlphaMode_Count; alphaMode = static_cast<AlphaMode>(alphaMode + 1))
    {
        D3D12_SHADER_BYTECODE& dxMeshShader = meshShader->bytecodes[alphaMode];

        IE_Check(m_Device->CreateRootSignature(0, dxMeshShader.pShaderBytecode, dxMeshShader.BytecodeLength, IID_PPV_ARGS(&pipelineState.rootSignatures[alphaMode])));

        D3D12_BLEND_DESC blendDesc;
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        for (u8 i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        {
            D3D12_RENDER_TARGET_BLEND_DESC& rtBlendDesc = blendDesc.RenderTarget[i];
            rtBlendDesc.LogicOpEnable = FALSE;
            rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
            rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            rtBlendDesc.BlendEnable = FALSE;
        }

        constexpr D3D12_RASTERIZER_DESC rasterizerDesc = {
            .FillMode = D3D12_FILL_MODE_SOLID,
            .CullMode = D3D12_CULL_MODE_BACK,
            .FrontCounterClockwise = FALSE,
            .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
            .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
            .DepthClipEnable = TRUE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
            .ForcedSampleCount = 0,
            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
        };

        constexpr D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {
            .DepthEnable = TRUE,
            .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,  // don't write during shading
            .DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL, // pass equal Z
            .StencilEnable = FALSE,
        };

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {
            .pRootSignature = pipelineState.rootSignatures[alphaMode].Get(),
            .AS = amplificationShader ? amplificationShader->bytecodes[alphaMode] : D3D12_SHADER_BYTECODE{},
            .MS = meshShader ? dxMeshShader : D3D12_SHADER_BYTECODE{},
            .PS = pixelShader ? pixelShader->bytecodes[alphaMode] : D3D12_SHADER_BYTECODE{},
            .BlendState = blendDesc,
            .SampleMask = UINT_MAX,
            .RasterizerState = rasterizerDesc,
            .DepthStencilState = depthStencilDesc,
            .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            .NumRenderTargets = renderTargets.Size(),
            .DSVFormat = depthStencil->resource[0]->GetDesc().Format,
            .SampleDesc = DefaultSampleDesc(),
        };
        for (u32 i = 0; i < renderTargets.Size(); ++i)
        {
            psoDesc.RTVFormats[i] = renderTargets[i]->resource[0]->GetDesc().Format;
        }
        CD3DX12_PIPELINE_MESH_STATE_STREAM psoStream = CD3DX12_PIPELINE_MESH_STATE_STREAM(psoDesc);
        const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {
            .SizeInBytes = sizeof(psoStream),
            .pPipelineStateSubobjectStream = &psoStream,
        };
        IE_Check(m_Device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState.pipelineStates[alphaMode])));
    }

    // Depth only
    D3D12_SHADER_BYTECODE& dxMeshShader = meshShader->bytecodes[AlphaMode_Opaque];

    IE_Check(m_Device->CreateRootSignature(0, dxMeshShader.pShaderBytecode, dxMeshShader.BytecodeLength, IID_PPV_ARGS(&pipelineState.depthRootSignature)));

    D3D12_BLEND_DESC blendDesc = {
        .AlphaToCoverageEnable = FALSE,
        .IndependentBlendEnable = FALSE,
    };
    for (u8 i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        D3D12_RENDER_TARGET_BLEND_DESC& rtBlendDesc = blendDesc.RenderTarget[i];
        rtBlendDesc.LogicOpEnable = FALSE;
        rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rtBlendDesc.BlendEnable = FALSE;
    }

    constexpr D3D12_RASTERIZER_DESC rasterizerDesc = {
        .FillMode = D3D12_FILL_MODE_SOLID,
        .CullMode = D3D12_CULL_MODE_BACK,
        .FrontCounterClockwise = FALSE,
        .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
        .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
        .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
        .DepthClipEnable = TRUE,
        .MultisampleEnable = FALSE,
        .AntialiasedLineEnable = FALSE,
        .ForcedSampleCount = 0,
        .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {
        .pRootSignature = pipelineState.depthRootSignature.Get(),
        .AS = amplificationShader ? amplificationShader->bytecodes[AlphaMode_Opaque] : D3D12_SHADER_BYTECODE{},
        .MS = meshShader ? dxMeshShader : D3D12_SHADER_BYTECODE{},
        .PS = pixelShader ? pixelShader->bytecodes[AlphaMode_Opaque] : D3D12_SHADER_BYTECODE{},
        .BlendState = blendDesc,
        .SampleMask = UINT_MAX,
        .RasterizerState = rasterizerDesc,
        .DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT),
        .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        .NumRenderTargets = renderTargets.Size(),
        .DSVFormat = depthStencil->resource[0]->GetDesc().Format,
        .SampleDesc = DefaultSampleDesc(),
    };
    for (u32 i = 0; i < renderTargets.Size(); ++i)
    {
        psoDesc.RTVFormats[i] = renderTargets[i]->resource[0]->GetDesc().Format;
    }
    CD3DX12_PIPELINE_MESH_STATE_STREAM psoStream = CD3DX12_PIPELINE_MESH_STATE_STREAM(psoDesc);
    const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {
        .SizeInBytes = sizeof(psoStream),
        .pPipelineStateSubobjectStream = &psoStream,
    };
    IE_Check(m_Device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState.depthPipelineState)));

    return IE_MakeSharedPtr<PipelineState>(pipelineState);
}

SharedPtr<Shader> Renderer::LoadShader(const ShaderType type, const WString& filename)
{
    Array<D3D12_SHADER_BYTECODE, AlphaMode_Count> bytecodes;

    WString defines;
    if (IE_Constants::enableRaytracedShadows)
    {
        defines += L"-D ENABLE_RAYTRACED_SHADOWS ";
    }

    IDxcBlob* resultOpaque = CompileShader(type, filename, {defines});
    bytecodes[AlphaMode_Opaque] = CD3DX12_SHADER_BYTECODE(resultOpaque->GetBufferPointer(), resultOpaque->GetBufferSize());

    const WString definesAlphaTestArg = defines + L"-D ENABLE_ALPHA_TEST ";
    IDxcBlob* resultAlphaTest = CompileShader(type, filename, {definesAlphaTestArg});
    bytecodes[AlphaMode_Mask] = CD3DX12_SHADER_BYTECODE(resultAlphaTest->GetBufferPointer(), resultAlphaTest->GetBufferSize());

    const WString definesBlendTestArg = defines + L"-D ENABLE_BLEND ";
    IDxcBlob* resultBlend = CompileShader(type, filename, {definesBlendTestArg});
    bytecodes[AlphaMode_Blend] = CD3DX12_SHADER_BYTECODE(resultBlend->GetBufferPointer(), resultBlend->GetBufferSize());

    const Shader dxShader = {
        .bytecodes = bytecodes,
        .filename = filename,
    };
    return IE_MakeSharedPtr<Shader>(dxShader);
}

void Renderer::GetOrWaitNextFrame()
{
    m_FrameInFlightIdx = m_Swapchain->GetCurrentBackBufferIndex();
    m_Pfd = &m_AllFrameData[m_FrameInFlightIdx];

    // Wait until the fence has been processed
    if (m_Pfd->renderFinishedFence->GetCompletedValue() < m_Pfd->renderFinishedFenceValue)
    {
        IE_Check(m_Pfd->renderFinishedFence->SetEventOnCompletion(m_Pfd->renderFinishedFenceValue, m_FenceEvent));
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }
    m_Pfd->renderFinishedFenceValue++;
}

void Renderer::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmdList, const SharedPtr<Buffer>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes) const
{
    // Create temporary upload buffer
    ComPtr<ID3D12Resource> uploadBuffer;
    AllocateUploadBuffer(data, sizeInBytes, offsetInBytes, uploadBuffer, L"SetBufferData/TempUploadBuffer");

    // Copy data from the upload buffer to the target buffer
    Barrier(cmdList, buffer->buffer, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(buffer->buffer.Get(), offsetInBytes, uploadBuffer.Get(), 0, sizeInBytes);
    Barrier(cmdList, buffer->buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
}

void Renderer::SetResourceBufferData(const ComPtr<ID3D12Resource>& buffer, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    u8* mappedData;
    IE_Check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)));
    memcpy(mappedData + offsetInBytes, data, sizeInBytes);
    buffer->Unmap(0, nullptr);
}

void Renderer::UploadTexture(const ComPtr<ID3D12GraphicsCommandList7>& cmdList, i32 width, i32 height, const void* data, u32 sizeInBytes, DXGI_FORMAT format)
{
    // Describe the texture resource
    const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 0);

    constexpr D3D12MA::ALLOCATION_DESC textureAllocDesc = {
        .HeapType = D3D12_HEAP_TYPE_DEFAULT,
    };
    D3D12MA::Allocation* allocation;
    ComPtr<ID3D12Resource> resource;
    IE_Check(m_Allocator->CreateResource(&textureAllocDesc, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &allocation, IID_PPV_ARGS(&resource)));

    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = format,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D =
            {
                .MipLevels = 1,
            },
    };

    const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = GetNewCBVSRVUAVHeapHandle();
    m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, srvHandle);

    const D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    constexpr D3D12MA::ALLOCATION_DESC uploadBufferAllocDesc = {
        .HeapType = D3D12_HEAP_TYPE_UPLOAD,
    };
    ComPtr<ID3D12Resource> uploadBuffer;
    D3D12MA::Allocation* uploadBufferAllocation;
    IE_Check(m_Allocator->CreateResource(&uploadBufferAllocDesc, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &uploadBufferAllocation, IID_PPV_ARGS(&uploadBuffer)));

    if (data)
    {
        IE_Assert(format == DXGI_FORMAT_R8G8B8A8_UNORM); // other format not implemented!
        const D3D12_SUBRESOURCE_DATA textureData = {
            .pData = data,
            .RowPitch = 4ll * width, // Assuming 4 bytes per pixel (RGBA)
            .SlicePitch = 4ll * width * height,
        };
        UpdateSubresources(cmdList.Get(), resource.Get(), uploadBuffer.Get(), 0, 0, 1, &textureData);
    }

    Barrier(cmdList, resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::GetNewCBVSRVUAVHeapHandle()
{
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_CBVSRVUAVHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += m_CBVSRVUAVHeapLastIdx * m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_CBVSRVUAVHeapLastIdx++;
    return srvHandle;
}

void Renderer::Barrier(const ComPtr<ID3D12GraphicsCommandList7>& commandList, const ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
    const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), stateBefore, stateAfter);
    commandList->ResourceBarrier(1, &barrier);
}

void Renderer::UAVBarrier(const ComPtr<ID3D12GraphicsCommandList7>& commandList, const ComPtr<ID3D12Resource>& resource)
{
    const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource.Get());
    commandList->ResourceBarrier(1, &barrier);
}

void Renderer::AllocateUploadBuffer(const void* pData, u32 sizeInBytes, u32 offsetInBytes, ComPtr<ID3D12Resource>& resource, const wchar_t* resourceName) const
{
    const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    D3D12MA::Allocation* allocation;
    constexpr D3D12MA::ALLOCATION_DESC allocDesc = {
        .HeapType = D3D12_HEAP_TYPE_UPLOAD,
    };
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &allocation, IID_PPV_ARGS(&resource)));
    IE_Check(resource->SetName(resourceName));
    if (pData)
    {
        SetResourceBufferData(resource, pData, sizeInBytes, offsetInBytes);
    }
}

void Renderer::AllocateUAVBuffer(u32 sizeInBytes, ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES initialResourceState, const wchar_t* resourceName) const
{
    const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::Allocation* allocation;
    constexpr D3D12MA::ALLOCATION_DESC allocDesc = {
        .HeapType = D3D12_HEAP_TYPE_DEFAULT,
    };
    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, initialResourceState, nullptr, &allocation, IID_PPV_ARGS(&resource)));
    IE_Check(resource->SetName(resourceName));
}