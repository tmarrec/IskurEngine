// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/RT_Shadows_blur_common.hlsli"

[RootSignature(ROOT_SIG)]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Blur(tid.xy, int2(1, 0));
}