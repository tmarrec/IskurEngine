// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj)
{
    float2 ndc = uv * 2 - 1;
    ndc.y = -ndc.y; // D3D
    float4 clip = float4(ndc, depth, 1);
    float4 worldH = mul(clip, invViewProj);
    return worldH.xyz / worldH.w;
}
