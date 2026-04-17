// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <dxgi1_6.h>

#include "AutoExposure.h"
#include "BindlessHeaps.h"
#include "Buffer.h"
#include "Camera.h"
#include "Constants.h"
#include "Culling.h"
#include "DLSS.h"
#include "Environments.h"
#include "GBuffer.h"
#include "GpuResource.h"
#include "Primitive.h"
#include "Raytracing.h"
#include "RenderDevice.h"
#include "RenderSceneTypes.h"
#include "SceneResources.h"
#include "Shader.h"
#include "Sky.h"
#include "Texture.h"
#include "Timings.h"
#include "common/IskurPackFormat.h"
#include "shaders/CPUGPU.h"

class Window;
struct LoadedScene;

class Renderer
{
  public:
    explicit Renderer(Window& window);

    void Init();
    void Terminate();

    void Render();
    void MarkInstancesDirty();

    SharedPtr<Buffer> CreateBuffer(ID3D12GraphicsCommandList7* cmd, const BufferCreateDesc& createDesc);
    void SetBufferData(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const SharedPtr<Buffer>& dst, const void* data, u32 sizeInBytes, u32 offsetInBytes = 0);

    const ComPtr<ID3D12Device14>& GetDevice() const;
    Window& GetWindow();
    const Window& GetWindow() const;
    Camera& GetCamera();
    const Camera& GetCamera() const;
    u32 GetStreamlineFrameIndex() const;

    using PerFrameData = RenderDevice::PerFrameData;
    PerFrameData& GetCurrentFrameData();

    Environment& GetCurrentEnvironment();
    const Environment& GetCurrentEnvironment() const;
    const Vector<String>& GetEnvironmentNames() const;
    const String& GetCurrentEnvironmentName() const;
    i32 GetCurrentEnvironmentIndex() const;
    void SetCurrentEnvironmentIndex(i32 index);

    void RequestShaderReload();
    void RequestSceneSwitch(const String& sceneFile);
    void RequestDLSSMode(DLSS::Mode mode);
    void RequestFrameGenerationEnabled(bool enabled);
    DLSS::Mode GetDLSSMode() const;
    bool IsFrameGenerationEnabled() const;
    u32 GetFrameGenerationPresentedFrames() const;
    bool HasPendingSceneSwitch() const;
    const String& GetPendingSceneFile() const;
    const String& GetCurrentSceneFile() const;
    const Vector<SceneUtils::SceneListEntry>& GetAvailableScenes() const;

  private:
    Window& m_Window;

    void ApplyTestInstanceMotion();
    void CreateSceneDepthSRVs();
    void InvalidateRuntimeDescriptorIndices();
    void PrepareRuntimeReload();
    void PresentLoadingFrame();
    void RecreateRuntimeFrameResources(bool recreateSkyCube);
    void SubmitDLSSCommonConstants(const Camera::FrameData& cameraFrameData, f32 jitterPxX, f32 jitterPxY, bool reset);
    void SubmitSceneUploadAndSync(PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);

    // Keep the device layer declared before allocator-backed resources so it outlives them.
    RenderDevice m_RenderDevice;
    BindlessHeaps m_BindlessHeaps;
    SceneResources m_SceneResources;

    struct DepthPreResources
    {
        Array<ComPtr<ID3D12PipelineState>, CullMode_Count> opaquePSO;
        ComPtr<ID3D12RootSignature> opaqueRootSig;
        Array<ComPtr<ID3D12PipelineState>, CullMode_Count> alphaTestPSO;
        ComPtr<ID3D12RootSignature> alphaTestRootSig;
        Array<DepthTexture, IE_Constants::frameInFlightCount> dsvs;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        SharedPtr<Shader> opaqueMeshShader;
        SharedPtr<Shader> alphaTestMeshShader;
        SharedPtr<Shader> alphaTestShader;
    } m_DepthPre{};

    struct GBufferResources
    {
        SharedPtr<Shader> amplificationShader;
        SharedPtr<Shader> meshShader;
        Array<SharedPtr<Shader>, AlphaMode_Count> pixelShaders;

        Array<GBuffer, IE_Constants::frameInFlightCount> gbuffers = {};
        Array<Array<ComPtr<ID3D12PipelineState>, CullMode_Count>, AlphaMode_Count> psos;
        Array<ComPtr<ID3D12RootSignature>, AlphaMode_Count> rootSigs;
    } m_GBuf{};

    struct DLSSRRGuideResources
    {
        SharedPtr<Shader> cs;
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;

        Array<Texture, IE_Constants::frameInFlightCount> diffuseAlbedo;
        Array<Texture, IE_Constants::frameInFlightCount> specularAlbedo;
    } m_DLSSRRGuides{};

    struct ToneMapResources
    {
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;

        Array<RenderTarget, IE_Constants::frameInFlightCount> sdrRt;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Array<RenderTarget, IE_Constants::frameInFlightCount> uiRt;
        ComPtr<ID3D12DescriptorHeap> uiRtvHeap;
        Array<RenderTarget, IE_Constants::frameInFlightCount> backBufferRt;
        ComPtr<ID3D12DescriptorHeap> backBufferRtvHeap;
        SharedPtr<Shader> vxShader;
        SharedPtr<Shader> pxShader;
        SharedPtr<Shader> composePxShader;
        ComPtr<ID3D12RootSignature> composeRootSig;
        ComPtr<ID3D12PipelineState> composePso;
    } m_Tonemap{};

    struct BloomResources
    {
        static constexpr u32 kMaxMipCount = 6;
        u32 mipCount = 0;
        Array<XMUINT2, kMaxMipCount> mipSizes{};
        Array<Array<Texture, kMaxMipCount>, IE_Constants::frameInFlightCount> downChain{};
        Array<Array<Texture, kMaxMipCount>, IE_Constants::frameInFlightCount> upChain{};

        ComPtr<ID3D12RootSignature> downsampleRootSig;
        ComPtr<ID3D12PipelineState> downsamplePso;
        SharedPtr<Shader> downsampleCs;

        ComPtr<ID3D12RootSignature> upsampleRootSig;
        ComPtr<ID3D12PipelineState> upsamplePso;
        SharedPtr<Shader> upsampleCs;
    } m_Bloom{};

    struct UpscaleResources
    {
        XMUINT2 renderSize{};
        XMUINT2 presentSize{};
        Array<Texture, IE_Constants::frameInFlightCount> outputs;
    } m_Upscale{};

    void CreateRTVs();
    void CreateDSV();

    void SetRenderAndPresentSize();

    void CreateUpscaleResources();
    void CreateBloomResources();
    void CreateGBufferPassResources();
    void CreateDLSSRRGuideResources();

    void CreateDepthPrePassPipelines(const Vector<String>& globalDefines);
    void CreateGBufferPassPipelines(const Vector<String>& globalDefines);
    void CreateDLSSRRGuidePassPipelines(const Vector<String>& globalDefines);
    void CreateBloomPassPipelines(const Vector<String>& globalDefines);
    void CreateToneMapPassPipelines(const Vector<String>& globalDefines);

    void ReloadRuntimeAndScene(const String& sceneFile);
    void ReloadRuntimeForUpscalingConfigChange();
    void LoadScene(const String& sceneFile);
    void ProcessPendingSceneSwitch();
    void BeginFrame(PerFrameData& frameData, ComPtr<ID3D12GraphicsCommandList7>& cmd, Camera::FrameData& cameraFrameData, f32& jitterNormX, f32& jitterNormY);
    void Pass_DepthPre(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_GBuffer(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_DLSSRRGuides(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_PathTrace(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Upscale(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_Bloom(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_Tonemap(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void Pass_ImGui(const ComPtr<ID3D12GraphicsCommandList7>& cmd, const Camera::FrameData& cameraFrameData);
    void Pass_PresentComposite(const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void EndFrame(const PerFrameData& frameData, const ComPtr<ID3D12GraphicsCommandList7>& cmd);
    void CheckFrameGenerationApiError();

    void WaitForGpuIdle();
    void ReloadShaders();

    u32 m_FrameInFlightIdx = 0;
    u32 m_FrameIndex = 0;
    u32 m_StreamlineFrameIndex = 0;

    SharedPtr<Buffer> m_ConstantsBuffer;
    u8* m_ConstantsCbMapped = nullptr;
    u32 m_ConstantsCbStride = 0;

    D3D12_VIEWPORT m_RenderViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_RenderRect = {0, 0, 0, 0};
    D3D12_VIEWPORT m_PresentViewport = {0, 0, 0, 0, 0, 0};
    D3D12_RECT m_PresentRect = {0, 0, 0, 0};

    TimingState m_GpuTimingState{};
    CpuTimers m_CpuTimers{};
    TimingState m_CpuTimingState{};

    Camera m_Camera;
    Raytracing m_Raytracing;
    Culling m_Culling;
    AutoExposure m_AutoExposure;
    Sky m_Sky;
    bool m_TestMovePrev = false;
    Vector<XMFLOAT4X4> m_TestBaseWorlds;
    DLSS::Mode m_DLSSMode = DLSS::Mode::Quality;
    DLSS::Mode m_PendingDLSSMode = DLSS::Mode::Quality;
    bool m_HasPendingDLSSModeChange = false;
    bool m_FrameGenerationEnabled = true;
    bool m_PendingFrameGenerationEnabled = true;
    bool m_HasPendingFrameGenerationChange = false;
    u32 m_PendingSceneSwitchDelayFrames = 0;

    Environments m_Environments;

    bool m_PendingShaderReload = false;
};
