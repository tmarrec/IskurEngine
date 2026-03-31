// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "DLSS.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include "Streamline.h"

namespace
{
sl::DLSSMode ToSlMode(DLSS::Mode mode)
{
    switch (mode)
    {
    case DLSS::Mode::Disabled:
        return sl::DLSSMode::eOff;
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

void ApplyRecommendedPresets(sl::DLSSOptions& options)
{
    options.dlaaPreset = sl::DLSSPreset::ePresetK;
    options.qualityPreset = sl::DLSSPreset::ePresetK;
    options.balancedPreset = sl::DLSSPreset::ePresetK;
    options.performancePreset = sl::DLSSPreset::ePresetK;
    options.ultraPerformancePreset = sl::DLSSPreset::ePresetF;
}

} // namespace

void DLSS::VerifyLoaded()
{
    bool dlssLoaded = false;
    Streamline::CheckResult(slIsFeatureLoaded(sl::kFeatureDLSS, dlssLoaded));
    IE_Assert(dlssLoaded);
}

void DLSS::GetOptimalSettings(u32 outputWidth, u32 outputHeight, Mode mode, OptimalSettings& outSettings)
{
    if (mode == Mode::Disabled)
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

    sl::DLSSOptions options{};
    ApplyRecommendedPresets(options);
    options.mode = ToSlMode(mode);
    options.outputWidth = outputWidth;
    options.outputHeight = outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.useAutoExposure = sl::Boolean::eFalse;

    sl::DLSSOptimalSettings settings{};
    Streamline::CheckResult(slDLSSGetOptimalSettings(options, settings));

    outSettings.renderWidth = settings.optimalRenderWidth;
    outSettings.renderHeight = settings.optimalRenderHeight;
    outSettings.renderWidthMin = settings.renderWidthMin;
    outSettings.renderHeightMin = settings.renderHeightMin;
    outSettings.renderWidthMax = settings.renderWidthMax;
    outSettings.renderHeightMax = settings.renderHeightMax;
    outSettings.sharpness = settings.optimalSharpness;
}

void DLSS::Evaluate(const EvaluateDesc& desc)
{
    IE_Assert(desc.mode != Mode::Disabled);

    sl::FrameToken* frame = nullptr;
    const uint32_t frameIndex = desc.frameIndex;
    Streamline::CheckResult(slGetNewFrameToken(frame, &frameIndex));

    const sl::ViewportHandle viewport(0u);

    sl::Resource colorIn(sl::ResourceType::eTex2d, desc.colorIn, static_cast<uint32_t>(desc.colorInState));
    colorIn.width = desc.renderWidth;
    colorIn.height = desc.renderHeight;
    colorIn.nativeFormat = static_cast<uint32_t>(desc.colorInFormat);
    colorIn.mipLevels = 1;
    colorIn.arrayLayers = 1;

    sl::Resource colorOut(sl::ResourceType::eTex2d, desc.colorOut, static_cast<uint32_t>(desc.colorOutState));
    colorOut.width = desc.outputWidth;
    colorOut.height = desc.outputHeight;
    colorOut.nativeFormat = static_cast<uint32_t>(desc.colorOutFormat);
    colorOut.mipLevels = 1;
    colorOut.arrayLayers = 1;

    sl::Resource depth(sl::ResourceType::eTex2d, desc.depth, static_cast<uint32_t>(desc.depthState));
    depth.width = desc.renderWidth;
    depth.height = desc.renderHeight;
    depth.nativeFormat = static_cast<uint32_t>(desc.depthFormat);
    depth.mipLevels = 1;
    depth.arrayLayers = 1;

    sl::Resource mvec(sl::ResourceType::eTex2d, desc.motionVectors, static_cast<uint32_t>(desc.motionVectorsState));
    mvec.width = desc.renderWidth;
    mvec.height = desc.renderHeight;
    mvec.nativeFormat = static_cast<uint32_t>(desc.motionVectorsFormat);
    mvec.mipLevels = 1;
    mvec.arrayLayers = 1;

    IE_Assert(desc.exposure != nullptr);
    sl::Resource exposure(sl::ResourceType::eTex2d, desc.exposure, static_cast<uint32_t>(desc.exposureState));
    exposure.width = 1;
    exposure.height = 1;
    exposure.nativeFormat = static_cast<uint32_t>(desc.exposureFormat);
    exposure.mipLevels = 1;
    exposure.arrayLayers = 1;

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
    sl::ResourceTag persistentTags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
    };

    Streamline::CheckResult(slSetTagForFrame(*frame, viewport, persistentTags, std::size(persistentTags), desc.cmd));

    sl::ResourceTag localColorInTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localColorOutTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent);
    sl::ResourceTag localMvecTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
    sl::ResourceTag localExposureTag(&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eOnlyValidNow);
    sl::DLSSOptions options{};
    ApplyRecommendedPresets(options);
    options.mode = ToSlMode(desc.mode);
    options.outputWidth = desc.outputWidth;
    options.outputHeight = desc.outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.useAutoExposure = sl::Boolean::eFalse;
    options.alphaUpscalingEnabled = sl::Boolean::eFalse;

    Streamline::CheckResult(slDLSSSetOptions(viewport, options));

    auto ToSl = [](const XMFLOAT4X4& m) -> sl::float4x4 {
        sl::float4x4 out{};
        out[0] = {m._11, m._12, m._13, m._14};
        out[1] = {m._21, m._22, m._23, m._24};
        out[2] = {m._31, m._32, m._33, m._34};
        out[3] = {m._41, m._42, m._43, m._44};
        return out;
    };

    const sl::float4x4 currentProj = ToSl(desc.projectionNoJitter);
    const sl::float4x4 prevProj = ToSl(desc.prevProjectionNoJitter);
    const sl::float4x4 invView = ToSl(desc.invView);
    const sl::float4x4 prevView = ToSl(desc.prevView);

    sl::Constants constants{};
    // cameraViewToClip must be projection-only (view-space -> clip-space), without jitter.
    constants.cameraViewToClip = currentProj;
    sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

    // Avoid deriving current->previous clip transforms from a full VP inverse.
    // Streamline provides a precision-safe helper for camera motion.
    sl::float4x4 prevCameraToWorld;
    sl::matrixFullInvert(prevCameraToWorld, prevView);
    sl::float4x4 cameraToPrevCamera;
    sl::calcCameraToPrevCamera(cameraToPrevCamera, invView, prevCameraToWorld);
    sl::float4x4 clipToPrevCameraView;
    sl::matrixMul(clipToPrevCameraView, constants.clipToCameraView, cameraToPrevCamera);
    sl::matrixMul(constants.clipToPrevClip, clipToPrevCameraView, prevProj);
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
    IE_Assert(IE_IsFinite(desc.cameraFar));
    IE_Assert(desc.cameraNear < desc.cameraFar);

    constants.cameraNear = desc.cameraNear;
    constants.cameraFar = desc.cameraFar;
    constants.cameraFOV = desc.cameraFov;
    constants.cameraAspectRatio = desc.cameraAspect;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.reset = desc.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    Streamline::CheckResult(slSetConstants(constants, *frame, viewport));

    const sl::BaseStructure* inputs[] = {
        &viewport, &localColorInTag, &localColorOutTag, &localMvecTag, &localExposureTag,
    };
    Streamline::CheckResult(slEvaluateFeature(sl::kFeatureDLSS, *frame, inputs, std::size(inputs), desc.cmd));
}
