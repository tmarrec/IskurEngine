// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

Texture2D<float> g_Input : register(t0);
Texture2D<float> g_Depth : register(t1);
RWTexture2D<float> g_Output : register(u0);
cbuffer CameraParams : register(b1)
{
    float zNear;
    float zFar;
};

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
    uint width, height;
    g_Input.GetDimensions(width, height);
    
    float rawD = g_Depth.Load(int3(uv,0));
    float cd = LinearizeDepth(rawD, zNear, zFar);
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

        float sd = LinearizeDepth(g_Depth.Load(int3(suv,0)), zNear, zFar);
        if (abs(sd - cd) <= depthDiffThreshold)
        {
            sum += weight * g_Input.Load(int3(suv,0));
            wsum += weight;
        }
    }

    g_Output[uv] = (wsum > 0) ? sum / wsum : g_Input.Load(int3(uv,0));
}