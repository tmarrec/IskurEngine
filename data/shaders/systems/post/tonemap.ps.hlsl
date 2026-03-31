// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<TonemapConstants> Constants : register(b0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"

float3 AgXCurve(float3 x)
{
    // AgX default contrast approximation polynomial.
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 AgXEncode(float3 color)
{
    const float3x3 AgXInputMat =
    {
        { 0.842479062253094, 0.0784336000000002, 0.0792237451477643 },
        { 0.0423282422610123, 0.878468636469772, 0.0791661274605434 },
        { 0.0423756549057051, 0.0784335999999992, 0.879142973793104 }
    };
    color = mul(AgXInputMat, max(color, 1e-6.xxx));

    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    return saturate((log2(color) - minEv) / (maxEv - minEv));
}

float3 AgXDecode(float3 color)
{
    const float3x3 AgXOutputMat =
    {
        { 1.19687900512017, -0.0980208811401368, -0.0990297440797205 },
        { -0.0528968517574562, 1.15190312990417, -0.0989611768448433 },
        { -0.0529716355144438, -0.0980434501171241, 1.15107367264116 }
    };

    color = AgXCurve(color);
    color = mul(AgXOutputMat, color);
    color = max(color, 0.0.xxx);

    // AgX output transfer approximation to display-linear.
    color = pow(color, 2.2.xxx);
    return saturate(color);
}

float ComputeSaturation(float3 color)
{
    float maxC = max(color.r, max(color.g, color.b));
    float minC = min(color.r, min(color.g, color.b));
    return (maxC > 1e-6f) ? ((maxC - minC) / maxC) : 0.0f;
}

float3 LinearToSRGB(float3 color)
{
    float3 lo = color * 12.92f;
    float3 hi = 1.055f * pow(max(color, 0.0.xxx), 1.0f / 2.4f) - 0.055f;
    return lerp(hi, lo, step(color, 0.0031308.xxx));
}

[RootSignature(ROOT_SIG)]
float4 main(VSOut input) : SV_TARGET
{
    Texture2D<float4> hdrTexture = ResourceDescriptorHeap[Constants.srvIndex];
    Texture2D<float4> bloomTexture = ResourceDescriptorHeap[Constants.bloomTextureIndex];
    SamplerState linearSampler = SamplerDescriptorHeap[Constants.samplerIndex];
    uint2 px = uint2(input.position.xy);
    float3 hdr = hdrTexture.Load(int3(px, 0)).rgb;
    float3 bloom = bloomTexture.SampleLevel(linearSampler, input.uv, 0).rgb * max(Constants.bloomIntensity, 0.0f);
    hdr += bloom;

    Texture2D<float> exposureTexture = ResourceDescriptorHeap[Constants.exposureTextureIndex];
    float exposure = exposureTexture.Load(int3(0, 0, 0));
    hdr *= max(exposure, 1e-6f);

    float3 ldr = AgXEncode(hdr);
    ldr = AgXDecode(ldr);
    ldr = saturate(ldr);

    const float pivot = 0.18;
    ldr = ((ldr - pivot) * Constants.contrast) + pivot;

    float3 lum = dot(ldr, float3(0.2126, 0.7152, 0.0722)).xxx;
    ldr = lerp(lum, ldr, Constants.saturation);

    ldr = saturate(LinearToSRGB(ldr));

    return float4(ldr, 1.0);
}
