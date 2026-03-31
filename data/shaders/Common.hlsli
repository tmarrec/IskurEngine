// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "include/core/math_constants.hlsli"

struct VertexOut
{
    float4 clipPos : SV_Position;
    float3 currentClipPosNoJ : TEXCOORD0; // xyw for motion vectors
    float3 prevClipPosNoJ : TEXCOORD1; // xyw for motion vectors
    float4 color : TEXCOORD2;
    uint meshletIndex : TEXCOORD3;
    float2 texCoord : TEXCOORD4;
    float3 T : TEXCOORD5;
    float3 N : TEXCOORD6;
    nointerpolation float Tw : TEXCOORD7;
};

struct Payload
{
    uint meshletIndices[32];
};

