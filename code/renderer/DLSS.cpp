// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "DLSS.h"

#include <atomic>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>

#include "Streamline.h"

namespace
{
constexpr f32 kFiniteStreamlineCameraFar = 1000000.0f;
std::atomic<i32> g_LastFrameGenerationApiError{S_OK};

sl::DLSSMode ToSlMode(DLSS::Mode mode)
{
    switch (mode)
    {
    case DLSS::Mode::Balanced:
        return sl::DLSSMode::eBalanced;
    case DLSS::Mode::Performance:
        return sl::DLSSMode::eMaxPerformance;
    case DLSS::Mode::UltraPerformance:
        return sl::DLSSMode::eUltraPerformance;
    case DLSS::Mode::DLAA:
        return sl::DLSSMode::eDLAA;
    case DLSS::Mode::Quality:
    default:
        return sl::DLSSMode::eMaxQuality;
    }
}

void ApplyRecommendedPresets(sl::DLSSDOptions& options)
{
    // RR is mandatory in the engine, so use the transformer preset family consistently.
    options.dlaaPreset = sl::DLSSDPreset::ePresetD;
    options.qualityPreset = sl::DLSSDPreset::ePresetD;
    options.balancedPreset = sl::DLSSDPreset::ePresetD;
    options.performancePreset = sl::DLSSDPreset::ePresetD;
    options.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
    options.ultraQualityPreset = sl::DLSSDPreset::ePresetD;
}

sl::FrameToken& GetFrameToken(const u32 streamlineFrameIndex)
{
    sl::FrameToken* frame = nullptr;
    const u32 frameIndex = streamlineFrameIndex;
    Streamline::CheckResult(slGetNewFrameToken(frame, &frameIndex));
    IE_Assert(frame != nullptr);
    return *frame;
}

sl::ViewportHandle GetViewport()
{
    return sl::ViewportHandle(0u);
}

sl::float4x4 ToSl(const XMFLOAT4X4& m)
{
    sl::float4x4 out{};
    out[0] = {m._11, m._12, m._13, m._14};
    out[1] = {m._21, m._22, m._23, m._24};
    out[2] = {m._31, m._32, m._33, m._34};
    out[3] = {m._41, m._42, m._43, m._44};
    return out;
}

f32 SanitizeCameraFar(const f32 cameraFar)
{
    if (!IE_IsFinite(cameraFar) || cameraFar >= (sl::INVALID_FLOAT * 0.5f))
    {
        return kFiniteStreamlineCameraFar;
    }
    return cameraFar;
}

void OnFrameGenerationApiError(const sl::APIError& lastError)
{
    // The callback runs from the present thread, so keep it write-only and let
    // the renderer surface the error on the main thread after present returns.
    g_LastFrameGenerationApiError.store(static_cast<i32>(lastError.hres), std::memory_order_release);
}

} // namespace

void DLSS::VerifyLoaded()
{
    bool dlssLoaded = false;
    bool dlssRRLoaded = false;
    Streamline::CheckResult(slIsFeatureLoaded(sl::kFeatureDLSS, dlssLoaded));
    Streamline::CheckResult(slIsFeatureLoaded(sl::kFeatureDLSS_RR, dlssRRLoaded));
    IE_Assert(dlssLoaded && dlssRRLoaded);
}

void DLSS::GetOptimalSettings(u32 outputWidth, u32 outputHeight, Mode mode, OptimalSettings& outSettings)
{
    if (mode == Mode::DLAA)
    {
        outSettings.renderWidth = outputWidth;
        outSettings.renderHeight = outputHeight;
        outSettings.renderWidthMin = outputWidth;
        outSettings.renderHeightMin = outputHeight;
        outSettings.renderWidthMax = outputWidth;
        outSettings.renderHeightMax = outputHeight;
        outSettings.sharpness = 0.0f;
        return;
    }

    sl::DLSSDOptions options{};
    ApplyRecommendedPresets(options);
    options.mode = ToSlMode(mode);
    options.outputWidth = outputWidth;
    options.outputHeight = outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;

    sl::DLSSDOptimalSettings settings{};
    Streamline::CheckResult(slDLSSDGetOptimalSettings(options, settings));

    outSettings.renderWidth = settings.optimalRenderWidth;
    outSettings.renderHeight = settings.optimalRenderHeight;
    outSettings.renderWidthMin = settings.renderWidthMin;
    outSettings.renderHeightMin = settings.renderHeightMin;
    outSettings.renderWidthMax = settings.renderWidthMax;
    outSettings.renderHeightMax = settings.renderHeightMax;
    outSettings.sharpness = settings.optimalSharpness;
}

void DLSS::SetCommonConstants(const CommonConstantsDesc& desc)
{
    sl::Constants constants{};
    // cameraViewToClip must be projection-only (view-space -> clip-space), without jitter.
    constants.cameraViewToClip = ToSl(desc.projectionNoJitter);
    sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

    // Avoid deriving current->previous clip transforms from a full VP inverse.
    // Streamline provides a precision-safe helper for camera motion.
    sl::float4x4 prevCameraToWorld;
    sl::matrixFullInvert(prevCameraToWorld, ToSl(desc.prevView));
    sl::float4x4 cameraToPrevCamera;
    sl::calcCameraToPrevCamera(cameraToPrevCamera, ToSl(desc.invView), prevCameraToWorld);
    sl::float4x4 clipToPrevCameraView;
    sl::matrixMul(clipToPrevCameraView, constants.clipToCameraView, cameraToPrevCamera);
    sl::matrixMul(constants.clipToPrevClip, clipToPrevCameraView, ToSl(desc.prevProjectionNoJitter));
    sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

    constants.clipToLensClip = sl::float4x4{
        sl::float4(1, 0, 0, 0),
        sl::float4(0, 1, 0, 0),
        sl::float4(0, 0, 1, 0),
        sl::float4(0, 0, 0, 1),
    };
    constants.jitterOffset = sl::float2(desc.jitterOffsetX, desc.jitterOffsetY);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(desc.cameraPos.x, desc.cameraPos.y, desc.cameraPos.z);
    constants.cameraRight = sl::float3(desc.invView._11, desc.invView._12, desc.invView._13);
    constants.cameraUp = sl::float3(desc.invView._21, desc.invView._22, desc.invView._23);
    constants.cameraFwd = sl::float3(desc.invView._31, desc.invView._32, desc.invView._33);

    IE_Assert(IE_IsFinite(desc.cameraNear));
    IE_Assert(desc.cameraNear < SanitizeCameraFar(desc.cameraFar));

    constants.cameraNear = desc.cameraNear;
    constants.cameraFar = SanitizeCameraFar(desc.cameraFar);
    constants.cameraFOV = desc.cameraFov;
    constants.cameraAspectRatio = desc.cameraAspect;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.reset = desc.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    Streamline::CheckResult(slSetConstants(constants, GetFrameToken(desc.streamlineFrameIndex), GetViewport()));
}

void DLSS::Evaluate(const EvaluateDesc& desc)
{
    IE_Assert(desc.exposure != nullptr);
    IE_Assert(desc.diffuseAlbedo != nullptr);
    IE_Assert(desc.specularAlbedo != nullptr);
    IE_Assert(desc.normalRoughness != nullptr);
    IE_Assert(desc.specularHitDistance != nullptr);

    sl::FrameToken& frame = GetFrameToken(desc.streamlineFrameIndex);
    const sl::ViewportHandle viewport = GetViewport();

    sl::Resource colorIn(sl::ResourceType::eTex2d, desc.colorIn, static_cast<u32>(desc.colorInState));
    colorIn.width = desc.renderWidth;
    colorIn.height = desc.renderHeight;
    colorIn.nativeFormat = static_cast<u32>(desc.colorInFormat);
    colorIn.mipLevels = 1;
    colorIn.arrayLayers = 1;

    sl::Resource colorOut(sl::ResourceType::eTex2d, desc.colorOut, static_cast<u32>(desc.colorOutState));
    colorOut.width = desc.outputWidth;
    colorOut.height = desc.outputHeight;
    colorOut.nativeFormat = static_cast<u32>(desc.colorOutFormat);
    colorOut.mipLevels = 1;
    colorOut.arrayLayers = 1;

    sl::Resource depth(sl::ResourceType::eTex2d, desc.depth, static_cast<u32>(desc.depthState));
    depth.width = desc.renderWidth;
    depth.height = desc.renderHeight;
    depth.nativeFormat = static_cast<u32>(desc.depthFormat);
    depth.mipLevels = 1;
    depth.arrayLayers = 1;

    sl::Resource mvec(sl::ResourceType::eTex2d, desc.motionVectors, static_cast<u32>(desc.motionVectorsState));
    mvec.width = desc.renderWidth;
    mvec.height = desc.renderHeight;
    mvec.nativeFormat = static_cast<u32>(desc.motionVectorsFormat);
    mvec.mipLevels = 1;
    mvec.arrayLayers = 1;

    const D3D12_RESOURCE_DESC exposureDesc = desc.exposure->GetDesc();
    sl::Resource exposure(sl::ResourceType::eTex2d, desc.exposure, static_cast<u32>(desc.exposureState));
    exposure.width = static_cast<u32>(exposureDesc.Width);
    exposure.height = static_cast<u32>(exposureDesc.Height);
    exposure.nativeFormat = static_cast<u32>(desc.exposureFormat);
    exposure.mipLevels = 1;
    exposure.arrayLayers = 1;

    sl::Resource diffuseAlbedo(sl::ResourceType::eTex2d, desc.diffuseAlbedo, static_cast<u32>(desc.diffuseAlbedoState));
    diffuseAlbedo.width = desc.renderWidth;
    diffuseAlbedo.height = desc.renderHeight;
    diffuseAlbedo.nativeFormat = static_cast<u32>(desc.diffuseAlbedoFormat);
    diffuseAlbedo.mipLevels = 1;
    diffuseAlbedo.arrayLayers = 1;

    sl::Resource specularAlbedo(sl::ResourceType::eTex2d, desc.specularAlbedo, static_cast<u32>(desc.specularAlbedoState));
    specularAlbedo.width = desc.renderWidth;
    specularAlbedo.height = desc.renderHeight;
    specularAlbedo.nativeFormat = static_cast<u32>(desc.specularAlbedoFormat);
    specularAlbedo.mipLevels = 1;
    specularAlbedo.arrayLayers = 1;

    sl::Resource normalRoughness(sl::ResourceType::eTex2d, desc.normalRoughness, static_cast<u32>(desc.normalRoughnessState));
    normalRoughness.width = desc.renderWidth;
    normalRoughness.height = desc.renderHeight;
    normalRoughness.nativeFormat = static_cast<u32>(desc.normalRoughnessFormat);
    normalRoughness.mipLevels = 1;
    normalRoughness.arrayLayers = 1;

    sl::Resource specularHitDistance(sl::ResourceType::eTex2d, desc.specularHitDistance, static_cast<u32>(desc.specularHitDistanceState));
    specularHitDistance.width = desc.renderWidth;
    specularHitDistance.height = desc.renderHeight;
    specularHitDistance.nativeFormat = static_cast<u32>(desc.specularHitDistanceFormat);
    specularHitDistance.mipLevels = 1;
    specularHitDistance.arrayLayers = 1;

    sl::Extent renderExtent;
    renderExtent.left = 0;
    renderExtent.top = 0;
    renderExtent.width = desc.renderWidth;
    renderExtent.height = desc.renderHeight;
    sl::Extent outputExtent;
    outputExtent.left = 0;
    outputExtent.top = 0;
    outputExtent.width = desc.outputWidth;
    outputExtent.height = desc.outputHeight;
    sl::Extent exposureExtent;
    exposureExtent.left = 0;
    exposureExtent.top = 0;
    exposureExtent.width = exposure.width;
    exposureExtent.height = exposure.height;
    sl::ResourceTag persistentTags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
    };

    Streamline::CheckResult(slSetTagForFrame(frame, viewport, persistentTags, std::size(persistentTags), desc.cmd));

    sl::ResourceTag localColorInTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localColorOutTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent);
    sl::ResourceTag localMvecTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localExposureTag(&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eOnlyValidNow, &exposureExtent);
    sl::ResourceTag localDiffuseAlbedoTag(&diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localSpecularAlbedoTag(&specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localNormalRoughnessTag(&normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localSpecularHitDistanceTag(&specularHitDistance, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);

    sl::DLSSDOptions options{};
    ApplyRecommendedPresets(options);
    options.mode = ToSlMode(desc.mode);
    options.outputWidth = desc.outputWidth;
    options.outputHeight = desc.outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
    options.alphaUpscalingEnabled = sl::Boolean::eFalse;
    options.cameraViewToWorld = ToSl(desc.invView);
    sl::matrixFullInvert(options.worldToCameraView, options.cameraViewToWorld);
    Streamline::CheckResult(slDLSSDSetOptions(viewport, options));

    const sl::BaseStructure* inputs[] = {
        &viewport,
        &localColorInTag,
        &localColorOutTag,
        &localMvecTag,
        &localExposureTag,
        &localDiffuseAlbedoTag,
        &localSpecularAlbedoTag,
        &localNormalRoughnessTag,
        &localSpecularHitDistanceTag,
    };
    Streamline::CheckResult(slEvaluateFeature(sl::kFeatureDLSS_RR, frame, inputs, std::size(inputs), desc.cmd));
}

void DLSS::SetFrameGenerationFeatureLoaded(const bool loaded)
{
    Streamline::CheckResult(slSetFeatureLoaded(sl::kFeatureDLSS_G, loaded));
}

void DLSS::SetFrameGenerationOptions(const FrameGenerationOptionsDesc& desc)
{
    sl::DLSSGOptions options{};
    options.mode = desc.enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    options.numBackBuffers = IE_Constants::frameInFlightCount;
    options.mvecDepthWidth = desc.renderWidth;
    options.mvecDepthHeight = desc.renderHeight;
    options.colorWidth = desc.outputWidth;
    options.colorHeight = desc.outputHeight;
    options.colorBufferFormat = static_cast<u32>(desc.backBufferFormat);
    options.mvecBufferFormat = static_cast<u32>(desc.motionVectorsFormat);
    options.depthBufferFormat = static_cast<u32>(desc.depthFormat);
    options.hudLessBufferFormat = static_cast<u32>(desc.hudlessFormat);
    options.uiBufferFormat = static_cast<u32>(desc.uiFormat);
    options.onErrorCallback = &OnFrameGenerationApiError;
    Streamline::CheckResult(slDLSSGSetOptions(GetViewport(), options));
}

void DLSS::TagFrameGenerationInputs(const FrameGenerationTagDesc& desc)
{
    IE_Assert(desc.cmd != nullptr);
    sl::FrameToken& frame = GetFrameToken(desc.streamlineFrameIndex);
    const sl::ViewportHandle viewport = GetViewport();

    if (!desc.valid)
    {
        sl::ResourceTag nullTags[] = {
            sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow),
        };
        Streamline::CheckResult(slSetTagForFrame(frame, viewport, nullTags, std::size(nullTags), desc.cmd));
        return;
    }

    IE_Assert(desc.depth != nullptr);
    IE_Assert(desc.motionVectors != nullptr);
    IE_Assert(desc.hudlessColor != nullptr);
    IE_Assert(desc.uiColorAndAlpha != nullptr);

    sl::Extent renderExtent{};
    renderExtent.width = desc.renderWidth;
    renderExtent.height = desc.renderHeight;
    sl::Extent outputExtent{};
    outputExtent.width = desc.outputWidth;
    outputExtent.height = desc.outputHeight;

    sl::Resource depth(sl::ResourceType::eTex2d, desc.depth, static_cast<u32>(desc.depthState));
    depth.width = desc.renderWidth;
    depth.height = desc.renderHeight;
    depth.nativeFormat = static_cast<u32>(desc.depthFormat);
    depth.mipLevels = 1;
    depth.arrayLayers = 1;

    sl::Resource motionVectors(sl::ResourceType::eTex2d, desc.motionVectors, static_cast<u32>(desc.motionVectorsState));
    motionVectors.width = desc.renderWidth;
    motionVectors.height = desc.renderHeight;
    motionVectors.nativeFormat = static_cast<u32>(desc.motionVectorsFormat);
    motionVectors.mipLevels = 1;
    motionVectors.arrayLayers = 1;

    sl::Resource hudless(sl::ResourceType::eTex2d, desc.hudlessColor, static_cast<u32>(desc.hudlessState));
    hudless.width = desc.outputWidth;
    hudless.height = desc.outputHeight;
    hudless.nativeFormat = static_cast<u32>(desc.hudlessFormat);
    hudless.mipLevels = 1;
    hudless.arrayLayers = 1;

    sl::Resource ui(sl::ResourceType::eTex2d, desc.uiColorAndAlpha, static_cast<u32>(desc.uiState));
    ui.width = desc.outputWidth;
    ui.height = desc.outputHeight;
    ui.nativeFormat = static_cast<u32>(desc.uiFormat);
    ui.mipLevels = 1;
    ui.arrayLayers = 1;

    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&motionVectors, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent),
    };
    Streamline::CheckResult(slSetTagForFrame(frame, viewport, tags, std::size(tags), desc.cmd));
}

void DLSS::GetFrameGenerationState(FrameGenerationState& outState)
{
    sl::DLSSGState state{};
    Streamline::CheckResult(slDLSSGGetState(GetViewport(), state, nullptr));
    outState.status = static_cast<u32>(state.status);
    outState.numFramesActuallyPresented = state.numFramesActuallyPresented;
}

bool DLSS::ConsumeFrameGenerationApiError(i32& outError)
{
    const i32 error = g_LastFrameGenerationApiError.exchange(S_OK, std::memory_order_acq_rel);
    if (error == S_OK)
    {
        return false;
    }

    outError = error;
    return true;
}
