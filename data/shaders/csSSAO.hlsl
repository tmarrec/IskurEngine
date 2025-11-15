// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"
#include "data/shaders/utils/EncodeDecodeNormal.hlsli"

ConstantBuffer<SSAOConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=58, b0)"

static const int sampleCount = 24;
static const float3 SSAOKernel[24] = {
    // Ring 1 (z≈0.15, scale 0.2)
    float3( 0.1977,  0.0000,  0.0300), float3( 0.0989,  0.1712,  0.0300),
    float3(-0.0989,  0.1712,  0.0300), float3(-0.1977,  0.0000,  0.0300),
    float3(-0.0989, -0.1712,  0.0300), float3( 0.0989, -0.1712,  0.0300),

    // Ring 2 (z≈0.40, scale 0.45)
    float3( 0.3574,  0.2062,  0.1800), float3( 0.0000,  0.4124,  0.1800),
    float3(-0.3574,  0.2062,  0.1800), float3(-0.3574, -0.2062,  0.1800),
    float3( 0.0000, -0.4124,  0.1800), float3( 0.3574, -0.2062,  0.1800),

    // Ring 3 (z≈0.70, scale 0.70)
    float3( 0.4999,  0.0000,  0.4900), float3( 0.2500,  0.4330,  0.4900),
    float3(-0.2500,  0.4330,  0.4900), float3(-0.4999,  0.0000,  0.4900),
    float3(-0.2500, -0.4330,  0.4900), float3( 0.2500, -0.4330,  0.4900),

    // Ring 4 (z≈0.90, scale 1.0)
    float3( 0.3770,  0.2179,  0.9000), float3( 0.0000,  0.4359,  0.9000),
    float3(-0.3770,  0.2179,  0.9000), float3(-0.3770, -0.2179,  0.9000),
    float3( 0.0000, -0.4359,  0.9000), float3( 0.3770, -0.2179,  0.9000)
};

// Hash for per-pixel random in [0,1)
static float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3x3 BuildTBN(in float3 N)
{
    N = normalize(N);
    float3 T = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = T - dot(T, N) * N; 
    T = normalize(T);
    float3 B = cross(N, T);
    
    return float3x3(
        T.x, T.y, T.z,
        B.x, B.y, B.z,
        N.x, N.y, N.z
    );
}

[RootSignature(ROOT_SIG)]
[NumThreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID,
                  uint3 gid : SV_GroupID,
                  uint3 gtid : SV_GroupThreadID)
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[Constants.depthTextureIndex];
    Texture2D<float2> normalTex = ResourceDescriptorHeap[Constants.normalTextureIndex];
    RWTexture2D<float> ssaoTex = ResourceDescriptorHeap[Constants.ssaoTextureIndex];
    SamplerState linearSampler = SamplerDescriptorHeap[Constants.samplerIndex];

    uint2 pix = tid.xy;
    float2 uv = float2(pix + 0.5) / Constants.renderTargetSize;

    float depthR = depthTex.Load(int3(pix,0));
    if (depthR <= 1e-6) return;

    // linear view-space depth (positive distance from camera)
    float viewZ = Constants.zNear / depthR;

    // position in view space: scale the ray so its z equals viewZ
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 vH = mul(float4(ndc, depthR, 1.0), Constants.invProj);
    float3 viewPos = vH.xyz / max(vH.w, 1e-6);

    // hemisphere sample in view-space
    float3 N_world = DecodeNormal(normalTex.Load(int3(pix,0)));
    float3 N_view = normalize(mul(float4(N_world,0), Constants.view).xyz);
    
    // Build tangent basis inline to avoid matrix mul per sample
    float3 T0 = (abs(N_view.z) < 0.999f) ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(T0 - dot(T0, N_view) * N_view);
    float3 B = cross(N_view, T);

    // Rotate tangent basis around N to break directional banding
    float angle = Hash12((float2)pix) * 6.2831853f;
    float s, c; sincos(angle, s, c);
    float3 Tr = c * T + s * B;
    float3 Br = -s * T + c * B;

    // Per-pixel invariants
    float px = 2.0 * viewZ / Constants.proj._11 / Constants.renderTargetSize.x;
    float py = 2.0 * viewZ / Constants.proj._22 / Constants.renderTargetSize.y;
    float pixelVS = 0.5 * (px + py);
    float biasVS = max(Constants.bias, 0.5 * pixelVS);

    float occlusion = 0.0f;
    for (int i = 0; i < sampleCount; ++i)
    {
        // Hemisphere sample in view space (rotate in tangent to reduce banding)
        float3 k = SSAOKernel[i];
        float3 sampleVec = Tr * k.x + Br * k.y + N_view * k.z;  // tangent -> view
        float3 samplePos = viewPos + sampleVec * Constants.radius;

        // project sample to screen
        float4 clipS = mul(float4(samplePos, 1.0f), Constants.proj);
        float invW = 1.0f / max(clipS.w, 1e-6);
        float2 uvS = float2(clipS.x * 0.5f * invW + 0.5f, 0.5f - clipS.y * 0.5f * invW);

        if (uvS.x <= 0.0f || uvS.x >= 1.0f || uvS.y <= 0.0f || uvS.y >= 1.0f)
        {
            continue;
        }

        uint2 sp = (uint2)(uvS * Constants.renderTargetSize);
        float sceneDepthR = depthTex.Load(int3(sp, 0));
        if (sceneDepthR <= 1e-6)
        {
            continue;
        }

        float sceneViewZ = Constants.zNear / sceneDepthR;
        float samplePosViewZ = -samplePos.z;
        float rangeCheck = smoothstep(0.0, 1.0, Constants.radius / abs(viewZ - sceneViewZ));
        occlusion += (sceneViewZ <= (samplePosViewZ - biasVS) ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0f - occlusion / sampleCount;
    ao = pow(saturate(ao), max(Constants.power, 1e-6));
    ssaoTex[pix] = ao;
}