// Iškur Engine
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
	XMFLOAT3 normal;
	XMFLOAT2 texCoord;
	XMFLOAT4 tangent;
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
};

struct
#ifdef __cplusplus
	alignas(256)
#endif
	VertexConstants
{
	XMFLOAT3 cameraPos;
	u32 unused;

	XMFLOAT4 planes[6];

	XMFLOAT4X4 view;
	XMFLOAT4X4 viewProj;
	XMFLOAT4X4 viewProjNoJ;
	XMFLOAT4X4 prevViewProjNoJ;
};

struct PrimitiveConstants
{
	XMFLOAT4X4 world;
	XMFLOAT4X4 worldIT;

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
	XMFLOAT3 cameraPos;

	XMFLOAT4X4 view;
	XMFLOAT4X4 invView;
	XMFLOAT4X4 invViewProj;

	XMFLOAT3 sunDir;
	u32 raytracingOutputIndex;

	u32 envMapIndex;
	u32 diffuseIBLIndex;
	u32 specularIBLIndex;
	u32 brdfLUTIndex;

	f32 sunAzimuth;
	f32 IBLSpecularIntensity;
	u32 RTShadowsEnabled;
	u32 ssaoTextureIndex;

	XMFLOAT2 renderSize;
	f32 sunIntensity;
	f32 skyIntensity;

	u32 aoTextureIndex;
	u32 indirectDiffuseTextureIndex;
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
	XMFLOAT4X4 invViewProj;

	u32 outputTextureIndex;
	u32 tlasIndex;
	XMUINT2 ditherOffset;

	XMFLOAT3 cameraPos;
	u32 depthTextureIndex;

	XMFLOAT2 fullDimInv;
	XMUINT2 ditherFactors;

	XMFLOAT3 sunDir;
	u32 unused;
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
	u32 indirectDiffuseTextureIndex;

	XMFLOAT3 sunDir;
	u32 normalGeoTextureIndex;

	XMFLOAT2 fullDimInv;
	u32 tlasIndex;
	u32 depthTextureIndex;

	u32 primInfoBufferIndex;
	u32 materialsBufferIndex;
	u32 radianceCacheUavIndex;
	u32 radianceCacheSrvIndex;

	u32 radianceSamplesUavIndex;
	u32 samplesCount;
	f32 radianceCacheCellSize;
	u32 frameIndex;

	u32 envMapIndex;
	f32 skyIntensity;
	u32 samplerIndex;
	f32 sunIntensity;

	u32 sppCached;
	u32 sppNotCached;
	u32 bounceCount;
	bool useTrilinear;

	u32 trilinearMinCornerSamples;
	u32 trilinearMinHits;
	u32 trilinearPresentMinSamples;
	u32 normalBinRes;

	u32 minExtraSppCount;
	u32 maxAge;
	u32 maxProbes;
	u32 maxSamples;

	f32 cellSize;
};

// Radiance cache

STATIC_C u32 RC_ENTRIES = 1u << 22;
STATIC_C u32 RC_MASK = RC_ENTRIES - 1;
STATIC_C u32 RC_EMPTY = 0xFFFFFFFFu;
STATIC_C u32 RC_LOCKED = 0xFFFFFFFEu;

struct RadianceCacheEntry
{
	u32 key; // 0xFFFFFFFF = empty
	u32 normalOct;
	u32 radianceR;
	u32 radianceG;
	u32 radianceB;
	u32 sampleCount;
	u32 lastFrame;
	u32 pad;
};

struct RadianceSample
{
	u32 key;
	u32 radianceR;
	u32 radianceG;
	u32 radianceB;
};

struct PathTraceCacheClearSamplesConstants
{
	u32 radianceSamplesUavIndex;
	u32 samplesCount;
};

struct PathTraceCacheIntegrateSamplesConstants
{
	u32 radianceSamplesSrvIndex;
	u32 radianceCacheUavIndex;
	u32 samplesCount;
	u32 frameIndex;

	u32 maxAge;
	u32 maxProbes;
	u32 maxSamples;
};

struct PathTraceCacheClearCacheConstants
{
	u32 radianceCacheUavIndex;
	u32 cacheEntries;
};