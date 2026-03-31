// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"
#include "include/rt/rt_shared.hlsli"
#include "include/geometry/reconstruct_world_pos.hlsli"

ConstantBuffer<PathTraceConstants> Constants : register(b0);

static const float FLOAT_SCALE = 8192.f;
static const float FULL_TRUST_SAMPLES = 256.f;
static const float PATH_TRACE_PRIMARY_BOUNCE_ORIGIN_BIAS = 0.025f;

bool AnyInvalid3(float3 v)
{
    return any(v != v) || any(abs(v) > 1e20.xxx);
}

uint Hash3(int3 p)
{
    uint x = RTHashUint((uint)p.x);
    uint y = RTHashUint((uint)p.y);
    uint z = RTHashUint((uint)p.z);
    return RTHashUint(x ^ (y * 0x9E3779B9u) ^ (z * 0x85EBCA6Bu));
}

int3 CellCoord(float3 pos, float cellSize)
{
    return (int3)floor(pos / cellSize);
}

float2 OctEncode(float3 n)
{
    n *= rcp(abs(n.x) + abs(n.y) + abs(n.z) + 1e-20f);

    float2 p = n.xy;
    if (n.z < 0.0f)
    {
        float2 s = float2((p.x >= 0.0f) ? 1.0f : -1.0f, (p.y >= 0.0f) ? 1.0f : -1.0f);
        p = (1.0f - abs(p.yx)) * s;
    }
    return p; // [-1,1]
}

float3 OctDecode(float2 e)
{
    float3 v = float3(e.xy, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f)
    {
        float2 s = float2((v.x >= 0.0f) ? 1.0f : -1.0f, (v.y >= 0.0f) ? 1.0f : -1.0f);
        v.xy = (1.0f - abs(v.yx)) * s;
    }
    return normalize(v);
}

uint NormalBin(float3 n)
{
    float2 uv = OctEncode(n) * 0.5f + 0.5f;
    uint2 q = (uint2)clamp((int2)floor(uv * (float)Constants.normalBinRes), 0, (int)Constants.normalBinRes - 1);
    return q.y * Constants.normalBinRes + q.x;
}

uint MakeKeyFromCellAndBin(int3 cell, uint normalBin)
{
    uint key = RTHashUint(Hash3(cell) ^ (normalBin * 0x27d4eb2du));
    if (key == RC_EMPTY)
    {
        key ^= 0xA511E9B3u;
    }
    if (key == RC_LOCKED)
    {
        key ^= 0x63D83595u;
    }
    return key;
}

uint MakeSignatureFromCellAndBin(int3 cell, uint normalBin)
{
    uint sig = Hash3(cell);
    sig = RTHashUint(sig ^ (normalBin * 0xB5297A4Du + 0x68E31DA4u));
    return sig;
}

uint MakeKeyFromCell(int3 cell, float3 n)
{
    return MakeKeyFromCellAndBin(cell, NormalBin(n));
}

uint MakeSignatureFromCell(int3 cell, float3 n)
{
    return MakeSignatureFromCellAndBin(cell, NormalBin(n));
}

uint MakeKey(float3 pos, float3 n)
{
    return MakeKeyFromCell(CellCoord(pos, Constants.radianceCacheCellSize), n);
}

uint MakeKeySignature(float3 pos, float3 n)
{
    return MakeSignatureFromCell(CellCoord(pos, Constants.radianceCacheCellSize), n);
}

float3 DecodeCacheRadiance(RadianceCacheEntry e)
{
    float invScale = rcp((float)e.sampleCount * FLOAT_SCALE);
    return float3((float)e.radianceR, (float)e.radianceG, (float)e.radianceB) * invScale;
}

bool CacheLookupKey(uint key, uint keySignature, out float3 outRadiance, out uint outCount)
{
    StructuredBuffer<RadianceCacheEntry> cache = ResourceDescriptorHeap[Constants.radianceCacheSrvIndex];
    uint idx = key & RC_MASK;

    outRadiance = 0.0.xxx;
    outCount = 0;

    [loop]
    for (uint p = 0; p < Constants.maxProbes; ++p)
    {
        uint i = (idx + p) & RC_MASK;
        RadianceCacheEntry e = cache[i];

        if (e.key == RC_LOCKED)
        {
            continue;
        }
        if (e.key == RC_EMPTY)
        {
            break;
        }
        if (e.key != key || e.normalOct != keySignature || e.sampleCount == 0)
        {
            continue;
        }
        outCount = e.sampleCount;
        outRadiance = DecodeCacheRadiance(e);
        return true;
    }

    return false;
}

bool CacheLookupCellSoftNormal(int3 cell, float3 n, out float3 outRadiance, out uint outCount)
{
    float2 uv = OctEncode(n) * 0.5f + 0.5f;
    float2 p = uv * (float)Constants.normalBinRes - 0.5f;
    int2 base = (int2)floor(p);
    float2 f = frac(p);

    const int binRes = (int)Constants.normalBinRes;
    int2 minBin = int2(0, 0);
    int2 maxBin = int2(binRes - 1, binRes - 1);

    float3 accum = 0.0.xxx;
    float cntSum = 0.0f;
    float wSum = 0.0f;
    float invSoftRange = rcp(max(1.0f - Constants.softNormalMinDot, 1e-4f));
    float minCountForBlend = max((float)Constants.minExtraSppCount, 1.0f);

    [unroll]
    for (uint y = 0; y < 2; ++y)
    {
        float wy = (y == 0) ? (1.0f - f.y) : f.y;

        [unroll]
        for (uint x = 0; x < 2; ++x)
        {
            float wx = (x == 0) ? (1.0f - f.x) : f.x;
            float w = wx * wy;

            int2 q = clamp(base + int2((int)x, (int)y), minBin, maxBin);
            uint bin = (uint)(q.y * binRes + q.x);
            uint key = MakeKeyFromCellAndBin(cell, bin);
            uint keySignature = MakeSignatureFromCellAndBin(cell, bin);

            // Seam-safe soft interpolation: don't blend bins whose decoded normal
            // points to the opposite hemisphere relative to the shading normal.
            float2 qUv = (float2((float)q.x, (float)q.y) + 0.5f) / (float)binRes;
            float3 qN = OctDecode(qUv * 2.0f - 1.0f);
            float nd = dot(qN, n);
            if (nd <= Constants.softNormalMinDot)
            {
                continue;
            }

            float3 r;
            uint cnt;
            if (!CacheLookupKey(key, keySignature, r, cnt))
            {
                continue;
            }

            // Confidence-aware soft blending:
            // - angular weight fades bins near the dot threshold
            // - sample weight suppresses noisy under-filled bins
            float angularW = saturate((nd - Constants.softNormalMinDot) * invSoftRange);
            float sampleW = saturate((float)cnt / minCountForBlend);
            float blendW = w * angularW * sampleW;
            if (blendW <= 0.0f)
            {
                continue;
            }

            accum += r * blendW;
            cntSum += (float)cnt * blendW;
            wSum += blendW;
        }
    }

    if (wSum <= 0.0f)
    {
        outRadiance = 0.0.xxx;
        outCount = 0u;
        return false;
    }

    outRadiance = accum / wSum;
    outCount = (uint)round(cntSum / max(wSum, 1e-6f));
    return true;
}

bool CacheLookupCellHardNormal(int3 cell, float3 n, out float3 outRadiance, out uint outCount)
{
    return CacheLookupKey(MakeKeyFromCell(cell, n), MakeSignatureFromCell(cell, n), outRadiance, outCount);
}

bool CacheLookup(float3 pos, float3 n, out float3 outRadiance, out uint outCount)
{
    int3 cell = CellCoord(pos, Constants.radianceCacheCellSize);
    if (Constants.useSoftNormalInterpolation != 0u)
    {
        return CacheLookupCellSoftNormal(cell, n, outRadiance, outCount);
    }

    return CacheLookupCellHardNormal(cell, n, outRadiance, outCount);
}

bool CacheLookupTrilinearSparse(float3 pos, float3 n, out float3 outRadiance, out uint outCount)
{
    float cellSize = Constants.radianceCacheCellSize;

    float3 cellF = pos / cellSize;
    int3 baseCell = (int3)floor(cellF);
    float3 f = frac(cellF);

    float3 accum = 0.0.xxx;
    float wSum = 0.0f;
    float cntSum = 0.0f;
    uint hits = 0;

    for (uint z = 0; z < 2; ++z)
    {
        float wz = (z == 0) ? (1.0f - f.z) : f.z;

        for (uint y = 0; y < 2; ++y)
        {
            float wy = (y == 0) ? (1.0f - f.y) : f.y;

            for (uint x = 0; x < 2; ++x)
            {
                float wx = (x == 0) ? (1.0f - f.x) : f.x;
                float w = wx * wy * wz;

                int3 cornerCell = baseCell + int3((int)x, (int)y, (int)z);

                float3 r;
                uint cnt;
                bool found = (Constants.useSoftNormalInterpolation != 0u) ? CacheLookupCellSoftNormal(cornerCell, n, r, cnt)
                                                                          : CacheLookupCellHardNormal(cornerCell, n, r, cnt);
                if (!found)
                {
                    continue;
                }

                if (cnt >= Constants.trilinearPresentMinSamples)
                {
                    hits++;
                }

                if (cnt >= Constants.trilinearMinCornerSamples)
                {
                    accum += r * w;
                    wSum += w;
                    cntSum += (float)cnt * w;
                }
            }
        }
    }

    if (hits < Constants.trilinearMinHits || wSum <= 0.0f)
    {
        return CacheLookup(pos, n, outRadiance, outCount);
    }

    outRadiance = accum / wSum;
    outCount = (uint)round(cntSum / max(wSum, 1e-6f));
    return true;
}

float CacheWeight(uint count)
{
    float x = saturate((float)count / FULL_TRUST_SAMPLES);
    return smoothstep(0.0f, 1.0f, x);
}

bool CacheLookupFiltered(float3 pos, float3 n, out float3 outRadiance, out uint outCount)
{
    if (Constants.useTrilinear)
    {
        return CacheLookupTrilinearSparse(pos, n, outRadiance, outCount);
    }
    else
    {
        return CacheLookup(pos, n, outRadiance, outCount);
    }
}

float TraceSunShadow(RaytracingAccelerationStructure scene, float3 hitPos, float3 hitN, float3 sunDir)
{
    RayDesc ray;
    ray.Origin = hitPos + hitN * (RT_RAY_EPS * Constants.radianceCacheCellSize);
    ray.Direction = -sunDir;
    ray.TMin = RT_RAY_EPS;
    ray.TMax = 1e6f;

    const uint flags =
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, flags, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (RTPassesAlphaTestExplicit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), Constants.primInfoBufferIndex,
                                          Constants.materialsBufferIndex))
            {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.indirectDiffuseTextureIndex];
    RWStructuredBuffer<RadianceSample> samples = ResourceDescriptorHeap[Constants.radianceSamplesUavIndex];

    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    uint fullW, fullH;
    depthTex.GetDimensions(fullW, fullH);

    uint2 px = DispatchRaysIndex().xy;
    uint2 fullPx = min(px * 2u + 1u, uint2(fullW - 1u, fullH - 1u));
    float2 uv = (float2(fullPx) + 0.5f) / float2(fullW, fullH);
    float depth = depthTex.Load(int3(fullPx, 0)).r;

    if (RTIsSkyDepth(depth))
    {
        output[px] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    Texture2D<float2> normalTex = ResourceDescriptorHeap[Constants.normalGeoTextureIndex];
    float3 n0 = DecodeNormal(normalTex.Load(int3(fullPx, 0)));

    float3 worldPos = ReconstructWorldPos(uv, depth, Constants.invViewProj);

    uint width = DispatchRaysDimensions().x;
    uint linearIdx = px.y * width + px.x;

    if (AnyInvalid3(worldPos) || AnyInvalid3(n0))
    {
        RadianceSample outInvalid = (RadianceSample)0;
        outInvalid.key = RC_EMPTY;
        samples[linearIdx] = outInvalid;
        output[px] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 cached;
    uint cacheCount;
    bool hasCache = CacheLookupFiltered(worldPos, n0, cached, cacheCount);
    float cacheW = (hasCache && cacheCount > Constants.minExtraSppCount) ? CacheWeight(cacheCount) : 0.0f;

    uint spp = Constants.spp;
    if (spp == 0u)
    {
        RadianceSample outNoSpp = (RadianceSample)0;
        outNoSpp.key = RC_EMPTY;
        samples[linearIdx] = outNoSpp;
        output[px] = float4(hasCache ? cached : 0.0.xxx, 1.0);
        return;
    }

    uint baseSeed = RTHashUint(px.x * 1973u + px.y * 9277u + 89173u + Constants.frameIndex * 26699u);
    SamplerState envSamp = SamplerDescriptorHeap[Constants.samplerIndex];
    TextureCube<float4> skyCube = ResourceDescriptorHeap[Constants.skyCubeIndex];

    float3 sunDir = Constants.sunDir;
    float3 sunL = -sunDir;
    float3 sunRad = Constants.sunRadiance;

    float3 sumRadiance = 0.0.xxx;

    for (uint s = 0; s < spp; ++s)
    {
        uint seed = baseSeed ^ (s * 0x9E3779B9u);

        float3 pos = worldPos;
        float3 n = n0;

        float3 throughput = 1.0.xxx;
        float3 radiance = 0.0.xxx;

        for (uint bounce = 0; bounce < Constants.bounceCount; ++bounce)
        {
            float3 wi = RTSampleCosineHemisphereWorld(float2(RTRand(seed), RTRand(seed)), n);

            RayPayload payload;
            const float originBias = (bounce == 0u) ? PATH_TRACE_PRIMARY_BOUNCE_ORIGIN_BIAS : RT_RAY_EPS;
            RTTracePayloadWithBiasAndTMin(scene, pos, n, wi, originBias, RT_RAY_EPS, payload);

            if (payload.hit == 0)
            {
                float3 sky = skyCube.SampleLevel(envSamp, wi, 0.0f).rgb;
                sky *= Constants.skyIntensity;
                radiance += throughput * sky;
                break;
            }

            float3 hitPos = payload.hitPos;
            float3 hitN = payload.hitN;
            float3 alb = payload.albedo;
            float3 emissive = payload.emissive;

            radiance += throughput * emissive;

            if (bounce == 0)
            {
                float NdotL = saturate(dot(hitN, sunL));
                float vis = 1.0f;
                if (NdotL > 0.0f)
                {
                    vis = TraceSunShadow(scene, hitPos, hitN, sunDir);
                }
                radiance += throughput * alb * (NdotL * vis) * sunRad * IE_INV_PI;
            }

            pos = hitPos;
            n = hitN;
            throughput *= alb;
        }

        sumRadiance += radiance;
    }

    float3 avgRadiance = sumRadiance / (float)spp;
    if (AnyInvalid3(avgRadiance))
    {
        avgRadiance = 0.0.xxx;
    }
    float safeEncodeMax = (4294967295.0f / max((float)Constants.maxSamples, 1.0f)) / FLOAT_SCALE;
    avgRadiance = min(max(avgRadiance, 0.0.xxx), float3(safeEncodeMax, safeEncodeMax, safeEncodeMax));
    float3 finalRadiance = lerp(avgRadiance, cached, cacheW);
    if (AnyInvalid3(finalRadiance))
    {
        finalRadiance = hasCache ? cached : 0.0.xxx;
    }

    RadianceSample outS;
    outS.key = MakeKey(worldPos, n0);
    outS.keySignature = MakeKeySignature(worldPos, n0);

    uint3 toAdd = (uint3)round(avgRadiance * FLOAT_SCALE);
    outS.radianceR = toAdd.x;
    outS.radianceG = toAdd.y;
    outS.radianceB = toAdd.z;

    samples[linearIdx] = outS;
    output[px] = float4(finalRadiance, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hit = 1;

    float t = RayTCurrent();
    float3 ro = WorldRayOrigin();
    float3 rd = WorldRayDirection();
    payload.hitPos = ro + rd * t;
    RTFillPayloadFromTriangleHit(payload, attr, rd, Constants.primInfoBufferIndex, Constants.materialsBufferIndex);
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (!RTPassesAlphaTest(attr, Constants.primInfoBufferIndex, Constants.materialsBufferIndex))
    {
        IgnoreHit();
    }
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
}

