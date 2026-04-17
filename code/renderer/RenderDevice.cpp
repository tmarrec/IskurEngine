// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "RenderDevice.h"

#include <sl.h>
#include <sl_helpers.h>

#include "BindlessHeaps.h"
#include "DLSS.h"
#include "Streamline.h"
#include "common/Asserts.h"
#include "common/CommandLineArguments.h"
#include "window/Window.h"

namespace
{
template <typename T> ComPtr<T> CreateStreamlineProxy(const ComPtr<T>& nativeInterface, const char* debugName)
{
    if (!nativeInterface)
    {
        return {};
    }

    T* proxyInterface = nativeInterface.Get();
    proxyInterface->AddRef();

    void* upgradedInterface = proxyInterface;
    const sl::Result upgradeRes = slUpgradeInterface(&upgradedInterface);
    if (upgradeRes != sl::Result::eOk || upgradedInterface == nullptr)
    {
        proxyInterface->Release();
        IE_LogError("Streamline proxy upgrade failed for {}: {}", debugName, sl::getResultAsStr(upgradeRes));
        IE_Assert(false);
        return {};
    }

    ComPtr<T> proxy;
    proxy.Attach(reinterpret_cast<T*>(upgradedInterface));
    return proxy;
}
} // namespace

void RenderDevice::Init(const Window& window, const XMUINT2& presentSize)
{
    CreateDevice();
    CreateAllocator();
    CreateCommandQueue();
    DLSS::SetFrameGenerationFeatureLoaded(true);
    CreateSwapchain(window, presentSize);
    CreateCommands();
    CreateFrameSynchronizationFences();
}

void RenderDevice::Terminate()
{
    IE_SetDeviceRemovedReasonDevice(nullptr);

    for (PerFrameData& frameData : m_AllFrameData)
    {
        if (frameData.fenceEvent)
        {
            CloseHandle(frameData.fenceEvent);
            frameData.fenceEvent = nullptr;
        }
    }
}

SharedPtr<Buffer> RenderDevice::CreateBuffer(BindlessHeaps& bindlessHeaps, ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc)
{
    IE_Assert(createDesc.sizeInBytes > 0);

    const bool hasInitData = (createDesc.initialData != nullptr) && (createDesc.initialDataSize > 0);

    if (hasInitData)
    {
        IE_Assert(createDesc.initialDataSize <= createDesc.sizeInBytes);
    }
    else
    {
        IE_Assert(createDesc.initialData == nullptr || createDesc.initialDataSize == 0);
    }

    if (createDesc.createSRV || createDesc.createUAV)
    {
        IE_Assert(createDesc.viewKind != BufferCreateDesc::ViewKind::None);
    }

    if (createDesc.createUAV)
    {
        IE_Assert(createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT);
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
    }

    if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0);
        IE_Assert(!createDesc.createUAV);
        IE_Assert(createDesc.initialState == D3D12_RESOURCE_STATE_COMMON || createDesc.initialState == D3D12_RESOURCE_STATE_GENERIC_READ);
        IE_Assert(createDesc.finalState == D3D12_RESOURCE_STATE_COMMON || createDesc.finalState == D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    else if (createDesc.heapType == D3D12_HEAP_TYPE_READBACK)
    {
        IE_Assert((createDesc.resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0);
        IE_Assert(!createDesc.createUAV);
        IE_Assert(!hasInitData);
        IE_Assert(createDesc.initialState == D3D12_RESOURCE_STATE_COMMON || createDesc.initialState == D3D12_RESOURCE_STATE_COPY_DEST);
        IE_Assert(createDesc.finalState == D3D12_RESOURCE_STATE_COMMON || createDesc.finalState == D3D12_RESOURCE_STATE_COPY_DEST);
    }

    SharedPtr<Buffer> out = IE_MakeSharedPtr<Buffer>();

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(createDesc.sizeInBytes, createDesc.resourceFlags);

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = createDesc.heapType;

    D3D12_RESOURCE_STATES createState = D3D12_RESOURCE_STATE_COMMON;
    if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        createState = D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    else if (createDesc.heapType == D3D12_HEAP_TYPE_READBACK)
    {
        createState = D3D12_RESOURCE_STATE_COMMON;
    }
    else if (createDesc.initialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        createState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }

    IE_Check(m_Allocator->CreateResource(&allocDesc, &resourceDesc, createState, nullptr, out->allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS(out->resource.ReleaseAndGetAddressOf())));

    out->SetName(createDesc.name);
    out->state = createState;

    D3D12_RESOURCE_STATES desiredState = createDesc.initialState;
    if (hasInitData && createDesc.finalState != D3D12_RESOURCE_STATE_COMMON)
    {
        desiredState = createDesc.finalState;
    }

    auto TransitionIfNeeded = [&](D3D12_RESOURCE_STATES newState) {
        if (!cmd || out->state == newState)
        {
            return;
        }

        out->Transition(cmd, newState);
    };

    if (hasInitData)
    {
        if (createDesc.heapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            u8* mapped = nullptr;
            const CD3DX12_RANGE readRange(0, 0);
            IE_Check(out->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped, createDesc.initialData, createDesc.initialDataSize);

            const CD3DX12_RANGE writeRange(0, createDesc.initialDataSize);
            out->Unmap(0, &writeRange);
        }
        else
        {
            IE_Assert(createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT);
            IE_Assert(cmd != nullptr);

            UploadTemp staging{};
            const D3D12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(createDesc.initialDataSize);

            D3D12MA::ALLOCATION_DESC upAlloc{};
            upAlloc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

            IE_Check(m_Allocator->CreateResource(&upAlloc, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, staging.allocation.ReleaseAndGetAddressOf(),
                                                 IID_PPV_ARGS(staging.resource.ReleaseAndGetAddressOf())));

            IE_Check(staging.resource->SetName(L"BufferInit/Upload"));

            u8* mapped = nullptr;
            const CD3DX12_RANGE readRange(0, 0);
            IE_Check(staging.resource->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped, createDesc.initialData, createDesc.initialDataSize);
            const CD3DX12_RANGE writeRange(0, createDesc.initialDataSize);
            staging.resource->Unmap(0, &writeRange);

            TransitionIfNeeded(D3D12_RESOURCE_STATE_COPY_DEST);

            cmd->CopyBufferRegion(out->Get(), 0, staging.resource.Get(), 0, createDesc.initialDataSize);

            if (desiredState != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                TransitionIfNeeded(desiredState);
            }

            TrackUpload(std::move(staging));
        }
    }
    else if (cmd && createDesc.heapType == D3D12_HEAP_TYPE_DEFAULT && createDesc.initialState != D3D12_RESOURCE_STATE_COMMON)
    {
        TransitionIfNeeded(createDesc.initialState);
    }

    out->srvIndex = UINT32_MAX;
    out->uavIndex = UINT32_MAX;
    out->numElements = 0;

    if (createDesc.viewKind == BufferCreateDesc::ViewKind::Structured)
    {
        IE_Assert(createDesc.strideInBytes > 0);
        IE_Assert((createDesc.strideInBytes & 3u) == 0);
        IE_Assert(createDesc.strideInBytes <= 2048u);
        IE_Assert((createDesc.sizeInBytes % createDesc.strideInBytes) == 0);

        const u64 n64 = createDesc.sizeInBytes / createDesc.strideInBytes;
        IE_Assert(n64 <= UINT32_MAX);
        const u32 n = static_cast<u32>(n64);

        out->numElements = n;

        if (createDesc.createSRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = n;
            srv.Buffer.StructureByteStride = createDesc.strideInBytes;
            out->srvIndex = bindlessHeaps.CreateSRV(out->resource, srv);
        }

        if (createDesc.createUAV)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = n;
            uav.Buffer.StructureByteStride = createDesc.strideInBytes;
            out->uavIndex = bindlessHeaps.CreateUAV(out->resource, uav);
        }
    }
    else if (createDesc.viewKind == BufferCreateDesc::ViewKind::Raw)
    {
        IE_Assert((createDesc.sizeInBytes & 3u) == 0);

        const u64 n64 = createDesc.sizeInBytes / 4u;
        IE_Assert(n64 <= UINT32_MAX);
        const u32 n = static_cast<u32>(n64);

        out->numElements = n;

        if (createDesc.createSRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = n;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            out->srvIndex = bindlessHeaps.CreateSRV(out->resource, srv);
        }

        if (createDesc.createUAV)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = n;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            out->uavIndex = bindlessHeaps.CreateUAV(out->resource, uav);
        }
    }
    else
    {
        IE_Assert(!createDesc.createSRV);
        IE_Assert(!createDesc.createUAV);
    }

    return out;
}

void RenderDevice::SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes)
{
    UploadTemp staging{};

    D3D12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    D3D12MA::ALLOCATION_DESC upAlloc{};
    upAlloc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    IE_Check(m_Allocator->CreateResource(&upAlloc, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, staging.allocation.ReleaseAndGetAddressOf(),
                                         IID_PPV_ARGS(staging.resource.ReleaseAndGetAddressOf())));
    IE_Check(staging.resource->SetName(L"SetBufferData/Upload"));

    u8* mapped = nullptr;
    IE_Check(staging.resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memcpy(mapped, data, sizeInBytes);
    staging.resource->Unmap(0, nullptr);

    const D3D12_RESOURCE_STATES before = dst->state;
    dst->Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(dst->Get(), offsetInBytes, staging.resource.Get(), 0, sizeInBytes);
    dst->Transition(cmd, before);

    TrackUpload(std::move(staging));
}

void RenderDevice::CreateGPUTimers(TimingState& timingState)
{
    IE_Check(m_CommandQueue->GetTimestampFrequency(&timingState.timestampFrequency));

    for (PerFrameData& frameData : m_AllFrameData)
    {
        constexpr u32 maxTimestamps = 256;

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = maxTimestamps;
        IE_Check(m_Device->CreateQueryHeap(&qh, IID_PPV_ARGS(&frameData.gpuTimers.heap)));

        const CD3DX12_RESOURCE_DESC rb = CD3DX12_RESOURCE_DESC::Buffer(maxTimestamps * sizeof(u64));
        const CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_READBACK);
        IE_Check(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&frameData.gpuTimers.readback)));
        IE_Check(frameData.gpuTimers.readback->SetName(L"GpuTimers Readback"));

        frameData.gpuTimers.nextIdx = 0;
        frameData.gpuTimers.passCount = 0;
    }
}

void RenderDevice::WaitForGpuIdle()
{
    ComPtr<ID3D12Fence> fence;
    IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    constexpr UINT64 value = 1;
    IE_Check(m_CommandQueue->Signal(fence.Get(), value));

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    IE_Check(fence->SetEventOnCompletion(value, evt));
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
}

void RenderDevice::WaitForFrame(PerFrameData& frameData)
{
    WaitOnFence(frameData.frameFence, frameData.frameFenceValue, frameData.fenceEvent);
}

void RenderDevice::ExecuteFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd, const u32 streamlineFrameIndex)
{
    IE_Check(cmd->Close());
    ID3D12CommandList* pCmdList = cmd.Get();

    Streamline::SetPCLMarker(sl::PCLMarker::eRenderSubmitStart, streamlineFrameIndex);
    m_CommandQueue->ExecuteCommandLists(1, &pCmdList);
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), frameData.frameFenceValue));
    Streamline::SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd, streamlineFrameIndex);

    constexpr UINT presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    Streamline::SetPCLMarker(sl::PCLMarker::ePresentStart, streamlineFrameIndex);
    IE_Check(m_Swapchain->Present(0, presentFlags));
    Streamline::SetPCLMarker(sl::PCLMarker::ePresentEnd, streamlineFrameIndex);
}

void RenderDevice::ExecuteAndWait(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd)
{
    IE_Check(cmd->Close());
    ID3D12CommandList* pCmdList = cmd.Get();
    m_CommandQueue->ExecuteCommandLists(1, &pCmdList);

    const u64 signalValue = frameData.frameFenceValue + 1;
    IE_Check(m_CommandQueue->Signal(frameData.frameFence.Get(), signalValue));
    if (frameData.frameFence->GetCompletedValue() < signalValue)
    {
        IE_Check(frameData.frameFence->SetEventOnCompletion(signalValue, frameData.fenceEvent));
        WaitForSingleObject(frameData.fenceEvent, INFINITE);
    }
    frameData.frameFenceValue = signalValue;
    ClearTrackedUploads(GetCurrentBackBufferIndex());
}

void RenderDevice::TrackUpload(UploadTemp&& upload)
{
    m_InFlightUploads[GetCurrentBackBufferIndex()].push_back(std::move(upload));
}

void RenderDevice::ClearTrackedUploads(u32 frameInFlightIdx)
{
    m_InFlightUploads[frameInFlightIdx].clear();
}

void RenderDevice::ClearAllTrackedUploads()
{
    for (Vector<UploadTemp>& uploads : m_InFlightUploads)
    {
        uploads.clear();
    }
}

u32 RenderDevice::GetCurrentBackBufferIndex() const
{
    IE_Assert(m_Swapchain != nullptr);
    return m_Swapchain->GetCurrentBackBufferIndex();
}

RenderDevice::PerFrameData& RenderDevice::GetCurrentFrameData()
{
    return m_AllFrameData[GetCurrentBackBufferIndex()];
}

Array<RenderDevice::PerFrameData, IE_Constants::frameInFlightCount>& RenderDevice::GetAllFrameData()
{
    return m_AllFrameData;
}

const ComPtr<ID3D12Device14>& RenderDevice::GetDevice() const
{
    return m_Device;
}

const ComPtr<D3D12MA::Allocator>& RenderDevice::GetAllocator() const
{
    return m_Allocator;
}

const ComPtr<ID3D12CommandQueue>& RenderDevice::GetCommandQueue() const
{
    return m_CommandQueue;
}

const ComPtr<IDXGISwapChain4>& RenderDevice::GetSwapchain() const
{
    return m_Swapchain;
}

void RenderDevice::WaitOnFence(const ComPtr<ID3D12Fence>& fence, u64& fenceValue, HANDLE& fenceEvent)
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        if (!fenceEvent)
        {
            fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            IE_Assert(fenceEvent != nullptr);
        }
        IE_Check(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    fenceValue++;
}

void RenderDevice::CreateDevice()
{
    const CommandLineArguments& args = GetCommandLineArguments();

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();

        if (args.gpuValidation)
        {
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(true);
            }
        }
    }
    m_Debug = debug;

    u32 factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#else
    u32 factoryFlags = 0;
#endif
    IE_Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_DxgiFactory)));
    m_DxgiFactoryProxy = CreateStreamlineProxy(m_DxgiFactory, "IDXGIFactory");

    ComPtr<IDXGIFactory6> factory6;
    IE_Check(m_DxgiFactory.As(&factory6));

    constexpr D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;

    for (u32 i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc;
        IE_Check(adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        ComPtr<ID3D12Device14> candidateDevice;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), featureLevel, IID_PPV_ARGS(&candidateDevice))))
        {
            m_Adapter = adapter;
            m_Device = candidateDevice;
            break;
        }
    }

    if (!m_Adapter || !m_Device)
    {
        IE_LogError("Failed to create a D3D12 device.");
        IE_Assert(false);
    }

    IE_SetDeviceRemovedReasonDevice(m_Device.Get());

    DXGI_ADAPTER_DESC1 adapterDesc{};
    IE_Check(m_Adapter->GetDesc1(&adapterDesc));
    Streamline::VerifyPCLSupport(adapterDesc.AdapterLuid);
    Streamline::VerifyReflexSupport(adapterDesc.AdapterLuid);
    Streamline::VerifyDLSSGSupport(adapterDesc.AdapterLuid);

#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_Device.As(&infoQueue)))
    {
        // Streamline DLSS-G lazily allocates an internal buffer with COPY_DEST as
        // the requested initial state. D3D12 ignores that state for buffers and
        // emits warning #1328, but the resource is valid. Keep breaking on
        // warnings generally, while filtering this known external warning.
        D3D12_MESSAGE_ID deniedMessageIds[] = {
            D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
        };
        D3D12_INFO_QUEUE_FILTER storageFilter{};
        storageFilter.DenyList.NumIDs = static_cast<UINT>(std::size(deniedMessageIds));
        storageFilter.DenyList.pIDList = deniedMessageIds;
        IE_Check(infoQueue->AddStorageFilterEntries(&storageFilter));

        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
    }
#endif

    Streamline::SetD3DDevice(m_Device.Get());
    Streamline::VerifyReflexLowLatencyAvailable();
    Streamline::VerifyDLSSGLoaded();
    DLSS::VerifyLoaded();
    m_DeviceProxy = CreateStreamlineProxy(m_Device, "ID3D12Device");
}

void RenderDevice::CreateAllocator()
{
    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED | D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;
    allocatorDesc.pDevice = m_Device.Get();
    allocatorDesc.pAdapter = m_Adapter.Get();
    IE_Check(D3D12MA::CreateAllocator(&allocatorDesc, &m_Allocator));
}

void RenderDevice::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    commandQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ID3D12Device14* queueDevice = m_DeviceProxy ? m_DeviceProxy.Get() : m_Device.Get();
    IE_Check(queueDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_CommandQueue)));
}

void RenderDevice::CreateSwapchain(const Window& window, const XMUINT2& presentSize)
{
    ComPtr<IDXGIFactory5> factory5;
    IE_Check(m_DxgiFactory.As(&factory5));

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
    swapchainDesc.Width = presentSize.x;
    swapchainDesc.Height = presentSize.y;
    swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = IE_Constants::frameInFlightCount;
    swapchainDesc.Scaling = DXGI_SCALING_NONE;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    IDXGIFactory6* swapchainFactory = m_DxgiFactoryProxy ? m_DxgiFactoryProxy.Get() : m_DxgiFactory.Get();
    ComPtr<IDXGISwapChain1> tempSwapchain;
    IE_Check(swapchainFactory->CreateSwapChainForHwnd(m_CommandQueue.Get(), window.GetHwnd(), &swapchainDesc, nullptr, nullptr, tempSwapchain.ReleaseAndGetAddressOf()));

    IE_Check(m_DxgiFactory->MakeWindowAssociation(window.GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
    IE_Check(tempSwapchain->QueryInterface(IID_PPV_ARGS(&m_Swapchain)));
}

void RenderDevice::CreateFrameSynchronizationFences()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_AllFrameData[i].frameFence)));
        m_AllFrameData[i].frameFenceValue = 0;
        m_AllFrameData[i].fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        IE_Assert(m_AllFrameData[i].fenceEvent != nullptr);
    }
}

void RenderDevice::CreateCommands()
{
    for (u32 i = 0; i < IE_Constants::frameInFlightCount; ++i)
    {
        IE_Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_AllFrameData[i].commandAllocator)));
        IE_Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_AllFrameData[i].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_AllFrameData[i].cmd)));
        IE_Check(m_AllFrameData[i].cmd->SetName(L"Main command list"));
        m_AllFrameData[i].cmd->Close();
    }
}
