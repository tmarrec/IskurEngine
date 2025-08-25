// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"

ConstantBuffer<TonemapConstants> Constants : register(b0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 ACESODT(float3 color)
{
    const float3x3 ACESInputMat =
    {
        { 0.59719, 0.35458, 0.04823 },
        { 0.07600, 0.90834, 0.01566 },
        { 0.02840, 0.13383, 0.83777 }
    };
    const float3x3 ACESOutputMat =
    {
        { 1.60475, -0.53108, -0.07367 },
        { -0.10208, 1.10813, -0.00605 },
        { -0.00327, -0.07276, 1.07602 }
    };
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

[RootSignature(ROOT_SIG)]
float4 main(VSOut input) : SV_TARGET
{
    Texture2D<float4> hdrTexture = ResourceDescriptorHeap[Constants.srvIndex];
    SamplerState hdrSampler = SamplerDescriptorHeap[Constants.samplerIndex];
    float3 hdr = hdrTexture.Sample(hdrSampler, input.uv).rgb;

    // Exposure
    StructuredBuffer<float> adaptExposureBuffer = ResourceDescriptorHeap[Constants.adaptExposureBufferIndex];
    float exp = adaptExposureBuffer[0];
    hdr *= exp;

    // White-point normalization + ACES
    hdr /= Constants.whitePoint;
    float3 ldr = ACESODT(hdr);
    ldr *= Constants.whitePoint;
    ldr = saturate(ldr);

    // Contrast around mid-gray
    const float pivot = 0.18;
    ldr = ((ldr - pivot) * Constants.contrast) + pivot;

    // Saturation
    float3 lum = dot(ldr, float3(0.2126, 0.7152, 0.0722)).xxx;
    ldr = lerp(lum, ldr, Constants.saturation);

    // Linear -> sRGB
    ldr = saturate(pow(ldr, 1.0 / 2.2));

    return float4(ldr, 1.0);
}