// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "compute/shadows/blur.hlsli"

[RootSignature(ROOT_SIG)]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Blur(tid.xy, int2(0, 1));
}