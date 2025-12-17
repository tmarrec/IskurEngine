// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<ClearConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=2, b0)"

[RootSignature(ROOT_SIG)]
[NumThreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> buffer = ResourceDescriptorHeap[Constants.bufferIndex];
    if (tid.x < Constants.numElements)
    {
        buffer[tid.x] = 0;
    }
}