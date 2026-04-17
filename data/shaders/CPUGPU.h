// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#ifdef __cplusplus
#include "common/Types.h"
#define STATIC_C static constexpr
#else
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

typedef float2 XMFLOAT2;
typedef float3 XMFLOAT3;
typedef float4 XMFLOAT4;
typedef float4x4 XMFLOAT4X4;

typedef uint2 XMUINT2;
typedef uint3 XMUINT3;
typedef uint4 XMUINT4;

#define STATIC_C static
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
	XMFLOAT3 center;
	f32 radius;

	/* normal cone, useful for backface culling */
	XMFLOAT3 cone_apex;
	XMFLOAT3 cone_axis;
	f32 cone_cutoff; /* = cos(angle/2) */

	i32 coneAxisAndCutoff;
};

struct Vertex
{
	XMFLOAT3 position;
	u32 normalPacked;
	u32 texCoordPacked;
	u32 colorPackedLo;
	u32 colorPackedHi;
	u32 tangentPacked;
};

struct Material
{
	f32 metallicFactor;
	f32 roughnessFactor;
	i32 baseColorTextureIndex;
	i32 baseColorSamplerIndex;

	XMFLOAT4 baseColorFactor;

	u32 alphaMode;
	f32 alphaCutoff;
	i32 metallicRoughnessTextureIndex;
	i32 metallicRoughnessSamplerIndex;

	i32 normalTextureIndex;
	i32 normalSamplerIndex;
	f32 normalScale;
	i32 doubleSided;

	i32 aoTextureIndex;
	i32 aoSamplerIndex;
	i32 emissiveTextureIndex;
	i32 emissiveSamplerIndex;
	XMFLOAT3 emissiveFactor;
	f32 _pad0;
};

struct
#ifdef __cplusplus
	alignas(256)
#endif
	VertexConstants
{
	XMFLOAT3 cameraPos;
	u32 gpuFrustumCullingEnabled;
	u32 gpuBackfaceCullingEnabled;
	f32 materialTextureMipBias;
	u32 _padVertexConstants0;
	u32 _padVertexConstants1;

	XMFLOAT4 planes[6];

	XMFLOAT4X4 view;
	XMFLOAT4X4 viewProj;
	XMFLOAT4X4 viewProjNoJ;
	XMFLOAT4X4 prevViewProjNoJ;
};

struct PrimitiveConstants
{
	XMFLOAT4X4 world;
	XMFLOAT4X4 prevWorld;
	XMFLOAT4X4 worldInv; // inverse (not transpose)

	u32 meshletCount;
	u32 materialIdx;
	u32 verticesBufferIndex;
	u32 meshletsBufferIndex;

	u32 meshletVerticesBufferIndex;
	u32 meshletTrianglesBufferIndex;
	u32 meshletBoundsBufferIndex;
	u32 materialsBufferIndex;

	u32 debugMeshletColorEnabled;
	f32 maxWorldScale;
	f32 worldSign;
	u32 allowBackfaceConeCull;
};

struct DLSSRRGuideConstants
{
	XMFLOAT4X4 invViewProj;
	XMFLOAT3 cameraPos;
	u32 albedoTextureIndex;
	u32 normalTextureIndex;
	u32 materialTextureIndex;
	u32 depthTextureIndex;
	u32 diffuseAlbedoOutputTextureIndex;
	u32 specularAlbedoOutputTextureIndex;
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
	u32 finalExposureTextureIndex;
	u32 resetHistory;
	f32 dt;
	f32 tauBright;
	f32 tauDark;
	f32 clampMin;
	f32 clampMax;
	f32 exposureCompensationEV;
};

struct TonemapConstants
{
	u32 srvIndex;
	u32 samplerIndex;
	u32 bloomTextureIndex;
	u32 exposureTextureIndex;
	f32 contrast;
	f32 saturation;
	f32 bloomIntensity;
};

struct PresentCompositeConstants
{
	u32 hudlessTextureIndex;
	u32 uiTextureIndex;
	u32 samplerIndex;
};

struct BloomDownsampleConstants
{
	u32 inputTextureIndex;
	u32 outputTextureIndex;
	u32 samplerIndex;
	u32 applyThreshold;
	f32 threshold;
	f32 softKnee;
};

struct BloomUpsampleConstants
{
	u32 baseTextureIndex;
	u32 bloomTextureIndex;
	u32 outputTextureIndex;
	u32 samplerIndex;
};

struct
#ifdef __cplusplus
	alignas(256)
#endif
	SkyMotionPassConstants
{
	u32 depthTextureIndex;
	u32 _pad0;
	u32 _pad1;
	u32 _pad2;

	XMFLOAT4X4 invView;
	XMFLOAT4X4 prevView;
	XMFLOAT4X4 projectionNoJitter;
	XMFLOAT4X4 prevProjectionNoJitter;
};

struct RTPrimInfo
{
	u32 vbSrvIndex;
	u32 ibSrvIndex;
	u32 materialIdx;
	u32 pad;
};

struct PathTraceConstants
{
	XMFLOAT4X4 invViewProj;

	XMFLOAT3 cameraPos;
	u32 outputTextureIndex;
	u32 hitDistanceTextureIndex;
	u32 depthTextureIndex;

	u32 albedoTextureIndex;
	u32 normalTextureIndex;
	u32 normalGeoTextureIndex;
	u32 materialTextureIndex;

	u32 emissiveTextureIndex;
	u32 tlasIndex;
	u32 primInfoBufferIndex;

	u32 materialsBufferIndex;
	u32 skyCubeIndex;
	u32 samplerIndex;
	// Keep sunDir + frameIndex together to match HLSL cbuffer packing for root constants.
	XMFLOAT3 sunDir;
	u32 frameIndex;

	XMFLOAT3 sunColor;
	u32 rtPathTraceSpp;

	XMFLOAT2 fullDimInv;
	f32 skyIntensity;
	f32 sunIntensity;
	f32 sunDiskAngleDeg;

	f32 shadowMinVisibility;
	f32 specularShadowMinVisibility;
	u32 rtMaxBounces;
	u32 _padPathTrace0;

	u32 rtShadowsEnabled;
	u32 rtUse2StateOmmRays;
	u32 rtOmmsEnabled;
	u32 rtSerEnabled;

	u32 debugViewMode;
	u32 debugMeshletColorEnabled;
	u32 _padPathTrace1;
	u32 _padPathTrace2;
};

STATIC_C u32 PathTraceDebugView_None = 0u;
STATIC_C u32 PathTraceDebugView_SurfaceNormals = 1u;

struct SkyCubeGenConstants
{
	u32 outUavIndex;
	u32 size;
	u32 _pad0;
	u32 _pad1;
	XMFLOAT3 sunDir;
	f32 sunIntensity;

	XMFLOAT3 sunColor;
	f32 sunDiskAngleDeg;

	f32 sunDiskSoftness;
	f32 sunGlowPower;
	f32 sunGlowIntensity;
	f32 sunDiskIntensityScale;

	f32 atmosphereThicknessKm;
	f32 atmosphereSunIntensityScale;
	f32 mieG;
	// Explicit cbuffer packing pad after float3-like boundary.
	f32 _padAfterMieG;

	XMFLOAT3 rayleighScattering;
	f32 rayleighScaleHeightKm;

	XMFLOAT3 mieScattering;
	f32 mieScaleHeightKm;

	XMFLOAT3 ozoneAbsorption;
	f32 ozoneLayerCenterKm;

	f32 ozoneLayerWidthKm;
	f32 multiScatteringStrength;
	// Keep struct dword-aligned for root constants upload.
	XMFLOAT2 _pad2;
};

