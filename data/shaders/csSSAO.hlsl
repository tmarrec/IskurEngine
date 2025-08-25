// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"
#include "data/shaders/utils/EncodeDecodeNormal.hlsli"

ConstantBuffer<SSAOConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=58, b0)"

static const int sampleCount = 32;
static const float3 SSAOKernel[32] = {
    float3( 0.2024, -0.0815,  0.0312), float3(-0.0758, -0.1956,  0.0205),
    float3(-0.0884,  0.2946,  0.0647), float3( 0.3185, -0.2533,  0.0896),
    float3(-0.4002,  0.1394,  0.1578), float3( 0.1292,  0.4756,  0.1823),
    float3( 0.5923, -0.3129,  0.3254), float3(-0.2045, -0.6233,  0.2896),
    float3(-0.7854,  0.2146,  0.4123), float3( 0.3865,  0.7429,  0.4534),
    float3( 0.2547, -0.8965,  0.5234), float3(-0.6754, -0.5023,  0.3865),
    float3( 0.9254,  0.1865,  0.6234), float3(-0.4123,  0.8956,  0.5896),
    float3(-0.3685, -0.9234,  0.6425), float3( 0.7854, -0.6123,  0.4896),
    float3( 0.0451,  0.0892,  0.7854), float3(-0.1254, -0.0685,  0.8234),
    float3( 0.0896,  0.2045,  0.9123), float3(-0.2234,  0.1685,  0.8756),
    float3( 0.1854, -0.2896,  0.9456), float3(-0.3254, -0.1896,  0.9234),
    float3( 0.2685,  0.3854,  0.8965), float3(-0.4123,  0.2896,  0.8234),
    float3( 0.3896, -0.4523,  0.7854), float3(-0.5234, -0.3685,  0.7456),
    float3( 0.4523,  0.5896,  0.6854), float3(-0.6123,  0.4523,  0.6234),
    float3( 0.5896, -0.6854,  0.5623), float3(-0.7234, -0.5896,  0.4956),
    float3( 0.6854,  0.7456,  0.4123), float3(-0.8123,  0.6854,  0.3254)
};

float3x3 BuildTBN(in float3 N)
{
    // Ensure N is normalized
    N = normalize(N);
    
    // Choose initial tangent vector
    float3 T = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    
    // Gram-Schmidt orthogonalization
    T = T - dot(T, N) * N;  // Remove component parallel to N
    T = normalize(T);
    
    // Bitangent completes the orthonormal basis
    float3 B = cross(N, T);
    
    return float3x3(
        T.x, T.y, T.z,
        B.x, B.y, B.z,
        N.x, N.y, N.z
    );
}

inline float ViewZ_FromReversedDepth(float d) { return Constants.zNear / max(d, 1e-6); }

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

    // view ray through this pixel (direction where z==1 in view space)
    float2 ndcXY = uv * 2.0f - 1.0f;
    float4 H = float4(ndcXY, 1.0f, 1.0f);
    float3 rayVS = mul(H, Constants.invProj).xyz;

    // position in view space: scale the ray so its z equals viewZ
    float depthNdc = depthR; // D3D NDC in [0,1]
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 vH = mul(float4(ndc, depthR, 1.0), Constants.invProj);
    float3 viewPos = vH.xyz / max(vH.w, 1e-6);

    // hemisphere sample in view-space
    float3 N_world = DecodeNormal(normalTex.Load(int3(pix,0)));
    float3 N_view = normalize(mul(float4(N_world,0), Constants.view).xyz);
    float3x3 TBN = BuildTBN(N_view);

    float occlusion = 0.0f;
    for (int i = 0; i < sampleCount; ++i)
    {
        // Hemisphere sample in view space
        float3 sampleVec = mul(SSAOKernel[i], TBN);  // tangent -> view
        float3 samplePos = viewPos + sampleVec * Constants.radius;

        // project sample to screen
        float4 clipS = mul(float4(samplePos, 1.0f), Constants.proj);
        float3 ndcS = clipS.xyz / max(clipS.w, 1e-6);
        float2 uvS = float2(ndcS.x * 0.5 + 0.5, 0.5 - ndcS.y * 0.5);

        if (any(uvS <= 0.0f) || any(uvS >= 1.0f)) continue;

        uint2 sp = (uint2)(uvS * Constants.renderTargetSize);
        float sceneDepthR = depthTex.Load(int3(sp, 0));
        if (sceneDepthR <= 1e-6) continue;

        // compare in the same metric (view-space Z, positive)
        float sceneViewZ = Constants.zNear / sceneDepthR;
        float samplePosViewZ = -samplePos.z;            // if your view uses -Z forward, use: -samplePos.z

        // pixel size (avg) in view-space at this depth
        float px = 2.0 * viewZ / Constants.proj._11 / Constants.renderTargetSize.x;
        float py = 2.0 * viewZ / Constants.proj._22 / Constants.renderTargetSize.y;
        float pixelVS = 0.5 * (px + py);

        // make bias at least a fraction of a pixel in view space
        float biasVS = max(Constants.bias, 0.5 * pixelVS);

        float rangeCheck = smoothstep(0.0, 1.0, Constants.radius / abs(viewZ - sceneViewZ));
        occlusion += (sceneViewZ <= (samplePosViewZ - biasVS) ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0f - occlusion / sampleCount;
    ao = pow(saturate(ao), max(Constants.power, 1e-6));
    ssaoTex[pix] = ao;
}