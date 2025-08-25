// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/CPUGPU.h"

ConstantBuffer<AdaptExposureConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=7, b0)"

[RootSignature(ROOT_SIG)]
[NumThreads(1, 1, 1)]
void main()
{
    StructuredBuffer<float> exposureBuffer = ResourceDescriptorHeap[Constants.exposureBufferIndex];
    RWStructuredBuffer<float> adaptedExposureBuffer = ResourceDescriptorHeap[Constants.adaptedExposureBufferIndex];

    float raw = exposureBuffer[0];
    raw = clamp(raw, Constants.clampMin, Constants.clampMax);

    float prev = adaptedExposureBuffer[0];

    bool needsBrighten = (raw > prev);
    float tau = needsBrighten ? Constants.tauBright : Constants.tauDark;

    float dt = max(Constants.dt, 0.0f);
    float k = 1.0f - exp(-dt / max(tau, 1e-3f));
    k = saturate(k);

    float next = lerp(prev, raw, k);
    next = clamp(next, Constants.clampMin, Constants.clampMax);

    adaptedExposureBuffer[0] = next;
}
