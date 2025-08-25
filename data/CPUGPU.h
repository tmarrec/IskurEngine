// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#ifdef __cplusplus
#include "../common/Types.h"
#else
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;
#endif

struct Meshlet
{
    u32 vertexOffset;
    u32 triangleOffset;
    u16 vertexCount;
    u16 triangleCount;
};

struct MeshletBounds
{
    /* bounding sphere, useful for frustum and occlusion culling */
    float3 center;
    f32 radius;

    /* normal cone, useful for backface culling */
    float3 cone_apex;
    float3 cone_axis;
    f32 cone_cutoff; /* = cos(angle/2) */

    i32 coneAxisAndCutoff;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

struct Material
{
    f32 metallicFactor;
    f32 roughnessFactor;
    i32 baseColorTextureIndex;
    i32 baseColorSamplerIndex;

    float4 baseColorFactor;

    u32 alphaMode;
    f32 alphaCutoff;
    i32 metallicRoughnessTextureIndex;
    i32 metallicRoughnessSamplerIndex;

    i32 normalTextureIndex;
    i32 normalSamplerIndex;
    f32 normalScale;
    i32 doubleSided;
};

struct
#ifdef __cplusplus
    alignas(256)
#endif
        VertexConstants
{
    float3 cameraPos;
    u32 unused;

    float4 planes[6];

    float4x4 view;
    float4x4 viewProj;
    float4x4 viewProjNoJ;
    float4x4 prevViewProjNoJ;
};

struct PrimitiveConstants
{
    float4x4 world;
    float4x4 worldIT;

    u32 meshletCount;
    u32 materialIdx;
    u32 verticesBufferIndex;
    u32 meshletsBufferIndex;

    u32 meshletVerticesBufferIndex;
    u32 meshletTrianglesBufferIndex;
    u32 meshletBoundsBufferIndex;
    u32 materialsBufferIndex;
};

struct
#ifdef __cplusplus
    alignas(256)
#endif
        LightingPassConstants
{
    u32 albedoTextureIndex;
    u32 normalTextureIndex;
    u32 materialTextureIndex;
    u32 depthTextureIndex;

    u32 samplerIndex;
    float3 cameraPos;

    float4x4 view;
    float4x4 invView;
    float4x4 invViewProj;

    float3 sunDir;
    u32 raytracingOutputIndex;

    u32 envMapIndex;
    u32 diffuseIBLIndex;
    u32 specularIBLIndex;
    u32 brdfLUTIndex;

    f32 sunAzimuth;
    f32 IBLDiffuseIntensity;
    f32 IBLSpecularIntensity;
    u32 RTShadowsEnabled;

    f32 RTShadowsIBLDiffuseStrength;
    f32 RTShadowsIBLSpecularStrength;
    float2 renderSize;

    u32 ssaoTextureIndex;
    f32 sunIntensity;
    f32 skyIntensity;
};

struct FXAAConstants
{
    float2 inverseRenderTargetSize;
    f32 edgeThreshold;
    f32 edgeThresholdMin;
    u32 ldrTextureIndex;
    u32 samplerIndex;
};

struct ClearConstants
{
    u32 bufferIndex;
    u32 numElements;
};

struct HistogramConstants
{
    u32 hdrTextureIndex;
    f32 minLogLum;
    f32 maxLogLum;
    u32 numBuckets;
    u32 histogramBufferIndex;
    u32 depthTextureIndex;
};

struct ExposureConstants
{
    u32 numBuckets;
    u32 totalPixels;
    f32 targetPct;
    f32 lowReject;
    f32 highReject;
    f32 key;
    f32 minLogLum;
    f32 maxLogLum;
    u32 histogramBufferIndex;
    u32 exposureBufferIndex;
};

struct AdaptExposureConstants
{
    u32 exposureBufferIndex;
    u32 adaptedExposureBufferIndex;
    f32 dt;
    f32 tauBright;
    f32 tauDark;
    f32 clampMin;
    f32 clampMax;
};

struct TonemapConstants
{
    u32 srvIndex;
    u32 samplerIndex;
    f32 whitePoint;
    f32 contrast;

    f32 saturation;
    u32 adaptExposureBufferIndex;
};

struct RTShadowsBlurConstants
{
    f32 zNear;
    f32 zFar;
    u32 inputTextureIndex;
    u32 depthTextureIndex;

    u32 outputTextureIndex;
};

struct RtShadowsTraceConstants
{
    float4x4 invViewProj;

    u32 outputTextureIndex;
    u32 tlasIndex;
    u32 depthSamplerIndex;
    u32 resolutionType;

    float3 sunDir;
    u32 frameIndex;

    float3 cameraPos;
    u32 depthTextureIndex;
};

struct SSAOConstants
{
    f32 radius;
    f32 bias;
    u32 depthTextureIndex;
    u32 normalTextureIndex;

    float4x4 proj;
    float4x4 invProj;
    float4x4 view;

    float2 renderTargetSize;
    u32 ssaoTextureIndex;
    u32 samplerIndex;

    f32 zNear;
    f32 power;
};