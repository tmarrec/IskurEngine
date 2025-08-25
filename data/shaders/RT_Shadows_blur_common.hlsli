// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=5, b0)"

ConstantBuffer<RTShadowsBlurConstants> Constants : register(b0);

static const int radius = 1;
static const float fadeStart = 5.f;
static const float fadeEnd = 50.f;
static const float blurW[3] = {0.25, 0.5, 0.25};
static const float idW[3] = {0.0, 1.0, 0.0};
static const float depthDiffThreshold = 0.01f;

float LinearizeDepth(float d, float zNear, float zFar)
{
    float zN = d * 2.0 - 1.0;
    return (2.0 * zNear * zFar) / (zFar + zNear - zN * (zFar - zNear));
}

void Blur(int2 uv, int2 axis)
{
    Texture2D<half> input = ResourceDescriptorHeap[Constants.inputTextureIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[Constants.depthTextureIndex];
    RWTexture2D<half> output = ResourceDescriptorHeap[Constants.outputTextureIndex];

    uint width, height;
    input.GetDimensions(width, height);
    
    float rawD = depth.Load(int3(uv,0));
    float cd = LinearizeDepth(rawD, Constants.zNear, Constants.zFar);
    float t = saturate((cd - fadeStart) / (fadeEnd - fadeStart));

    float w[3];
    w[0] = lerp(blurW[0], idW[0], t);
    w[1] = lerp(blurW[1], idW[1], t);
    w[2] = lerp(blurW[2], idW[2], t);

    float sum = 0;
    float wsum = 0;

    [unroll]
    for (int i = -radius; i <= radius; i++)
    {
        float weight = w[i + radius];
        int2 suv = uv + axis * i;
        suv = clamp(suv, int2(0,0), int2(width-1, height-1));

        float sd = LinearizeDepth(depth.Load(int3(suv,0)), Constants.zNear, Constants.zFar);
        if (abs(sd - cd) <= depthDiffThreshold)
        {
            sum += weight * input.Load(int3(suv,0));
            wsum += weight;
        }
    }

    output[uv] = half((wsum > 0) ? sum / wsum : input.Load(int3(uv,0)));
}