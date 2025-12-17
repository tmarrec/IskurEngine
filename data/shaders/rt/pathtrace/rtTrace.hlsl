// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Common.hlsli"
#include "CPUGPU.h"
#include "utils/ReconstructWorldPos.hlsli"
#include "utils/EncodeDecodeNormal.hlsli"

ConstantBuffer<PathTraceConstants> Constants : register(b0);

static const float INV_PI = 0.31830988618f;
static const float TWO_PI = 6.28318530718f;
static const float RAY_EPS = 0.001f;
static const float FLOAT_SCALE = 8192.f;
static const float FULL_TRUST_SAMPLES = 256.f;

struct [raypayload] RayPayload
{
    uint hit : read(caller, closesthit) : write(caller, closesthit);
    float3 hitPos : read(caller, closesthit) : write(caller, closesthit);
    float3 hitN : read(caller, closesthit) : write(caller, closesthit);
    float3 albedo : read(caller, closesthit) : write(caller, closesthit);
};

bool IsSkyDepth(float depth)
{
    return depth <= 1e-6f; // reversed-Z clear=0
}

uint HashUint(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint Hash3(int3 p)
{
    uint x = HashUint((uint)p.x);
    uint y = HashUint((uint)p.y);
    uint z = HashUint((uint)p.z);
    return HashUint(x ^ (y * 0x9E3779B9u) ^ (z * 0x85EBCA6Bu));
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

uint NormalBin(float3 n)
{
    float2 uv = OctEncode(n) * 0.5f + 0.5f;
    uint2 q = (uint2)clamp((int2)floor(uv * (float)Constants.normalBinRes), 0, (int)Constants.normalBinRes - 1);
    return q.y * Constants.normalBinRes + q.x;
}

uint MakeKeyFromCell(int3 cell, float3 n)
{
    uint key = HashUint(Hash3(cell) ^ (NormalBin(n) * 0x27d4eb2du));
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

uint MakeKey(float3 pos, float3 n)
{
    return MakeKeyFromCell(CellCoord(pos, Constants.radianceCacheCellSize), n);
}

float3 DecodeCacheRadiance(RadianceCacheEntry e)
{
    float invScale = rcp((float)e.sampleCount * FLOAT_SCALE);
    return float3((float)e.radianceR, (float)e.radianceG, (float)e.radianceB) * invScale;
}

bool CacheLookupKey(uint key, out float3 outRadiance, out uint outCount)
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
        if (e.key != key || e.sampleCount == 0)
        {
            continue;
        }
        if ((Constants.frameIndex - e.lastFrame) > Constants.maxAge)
        {
            continue;
        }

        outCount = e.sampleCount;
        outRadiance = DecodeCacheRadiance(e);
        return true;
    }

    return false;
}

bool CacheLookup(float3 pos, float3 n, out float3 outRadiance, out uint outCount)
{
    return CacheLookupKey(MakeKey(pos, n), outRadiance, outCount);
}

float Rand(inout uint seed)
{
    seed = HashUint(seed);
    return (seed & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float3 SampleCosineHemisphere(float2 u)
{
    float r = sqrt(u.x);
    float a = TWO_PI * u.y;
    return float3(r * cos(a), r * sin(a), sqrt(max(0.0f, 1.0f - u.x)));
}

// Duff et al. (Pixar): Building an Orthonormal Basis, Revisited
// n must be normalized.
void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;

    t = float3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    b = float3(bb, sign + n.y * n.y * a, -n.y);
}

float3 SampleCosineHemisphereWorld(float2 u, float3 n)
{
    float3 t, b;
    BuildOrthonormalBasis(n, t, b);

    float3 l = SampleCosineHemisphere(u);
    return t * l.x + b * l.y + n * l.z;
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

                uint key = MakeKeyFromCell(baseCell + int3((int)x, (int)y, (int)z), n);

                float3 r;
                uint cnt;
                if (!CacheLookupKey(key, r, cnt))
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
    ray.Origin = hitPos + hitN * (RAY_EPS * Constants.radianceCacheCellSize);
    ray.Direction = -sunDir;
    ray.TMin = RAY_EPS;
    ray.TMax = 1e6f;

    const uint flags =
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
        RAY_FLAG_FORCE_OPAQUE;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, flags, 0xFF, ray);
    while (q.Proceed())
    {
    }

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

[shader("raygeneration")]
void Raygen()
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[Constants.tlasIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[Constants.indirectDiffuseTextureIndex];
    RWStructuredBuffer<RadianceSample> samples = ResourceDescriptorHeap[Constants.radianceSamplesUavIndex];

    uint2 px = DispatchRaysIndex().xy;
    float2 uv = ((float2)px + 0.5f) * Constants.fullDimInv;

    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    float depth = depthTex.Load(int3(px, 0)).r;

    if (IsSkyDepth(depth))
    {
        output[px] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    Texture2D<float2> normalTex = ResourceDescriptorHeap[Constants.normalGeoTextureIndex];
    float3 n0 = normalize(DecodeNormal(normalTex.Load(int3(px, 0))));

    float3 worldPos = ReconstructWorldPos(uv, depth, Constants.invViewProj);

    float3 cached;
    uint cacheCount;
    bool hasCache = CacheLookupFiltered(worldPos, n0, cached, cacheCount);
    float cacheW = (hasCache && cacheCount > Constants.minExtraSppCount) ? CacheWeight(cacheCount) : 0.0f;

    uint spp = (!hasCache || cacheCount < Constants.maxSamples) ? Constants.sppNotCached : Constants.sppCached;

    uint baseSeed = HashUint(px.x * 1973u + px.y * 9277u + 89173u + Constants.frameIndex * 26699u);

    SamplerState envSamp = SamplerDescriptorHeap[Constants.samplerIndex];
    TextureCube<float4> envMap = ResourceDescriptorHeap[Constants.envMapIndex];

    float3 sunDir = normalize(Constants.sunDir);
    float3 sunL = -sunDir;
    float3 sunColor = float3(1.0f, 0.98f, 0.92f);
    float3 sunRad = sunColor * Constants.sunIntensity;

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
            float3 wi = SampleCosineHemisphereWorld(float2(Rand(seed), Rand(seed)), n);

            RayPayload payload;
            payload.hit = 0;

            RayDesc ray;
            ray.Origin = pos + n * RAY_EPS;
            ray.Direction = wi;
            ray.TMin = RAY_EPS;
            ray.TMax = 1e6f;

            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

            if (payload.hit == 0)
            {
                float3 sky = envMap.SampleLevel(envSamp, wi, 0.0f).rgb * Constants.skyIntensity;
                radiance += throughput * sky;
                break;
            }

            float3 hitPos = payload.hitPos;
            float3 hitN = payload.hitN;
            float3 alb = payload.albedo;

            if (bounce == 0)
            {
                float vis = TraceSunShadow(scene, hitPos, hitN, sunDir);
                float NdotL = saturate(dot(hitN, sunL));
                radiance += throughput * alb * (NdotL * vis) * sunRad * INV_PI;
            }

            pos = hitPos;
            n = hitN;
            throughput *= alb;
        }

        sumRadiance += radiance;
    }

    float3 avgRadiance = sumRadiance / (float)spp;
    float3 finalRadiance = lerp(avgRadiance, cached, cacheW);

    uint width = DispatchRaysDimensions().x;
    uint linearIdx = px.y * width + px.x;

    RadianceSample outS;
    outS.key = MakeKey(worldPos, n0);

    uint3 toAdd = (uint3)round(max(avgRadiance, 0.0.xxx) * FLOAT_SCALE);
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

    StructuredBuffer<RTPrimInfo> primInfos = ResourceDescriptorHeap[Constants.primInfoBufferIndex];
    RTPrimInfo info = primInfos[InstanceID()];

    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[info.vbSrvIndex];
    StructuredBuffer<uint> ib = ResourceDescriptorHeap[info.ibSrvIndex];

    uint tri = PrimitiveIndex();
    uint i0 = ib[tri * 3 + 0];
    uint i1 = ib[tri * 3 + 1];
    uint i2 = ib[tri * 3 + 2];

    Vertex v0 = vb[i0];
    Vertex v1 = vb[i1];
    Vertex v2 = vb[i2];

    float b1 = attr.barycentrics.x;
    float b2 = attr.barycentrics.y;
    float b0 = 1.0f - b1 - b2;

    float3 nObj = normalize(v0.normal * b0 + v1.normal * b1 + v2.normal * b2);

    float3x3 w2o = (float3x3)WorldToObject3x4();
    float3 nWorld = normalize(mul(transpose(w2o), nObj));
    if (dot(nWorld, -rd) < 0.0f)
    {
        nWorld = -nWorld;
    }

    payload.hitN = nWorld;

    float2 uv = v0.texCoord * b0 + v1.texCoord * b1 + v2.texCoord * b2;

    StructuredBuffer<Material> mats = ResourceDescriptorHeap[Constants.materialsBufferIndex];
    Material m = mats[info.materialIdx];

    float4 baseColor = m.baseColorFactor;
    if (m.baseColorTextureIndex != -1)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[m.baseColorTextureIndex];
        SamplerState samp = SamplerDescriptorHeap[m.baseColorSamplerIndex];
        baseColor *= tex.SampleLevel(samp, uv, 0.0f);
    }

    payload.albedo = baseColor.rgb;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
}
