// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<AdaptExposureConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=10, b0)"

[RootSignature(ROOT_SIG)]
[numthreads(1, 1, 1)]
void main()
{
    Texture2D<float> exposureTexture = ResourceDescriptorHeap[Constants.exposureBufferIndex];
    RWTexture2D<float> adaptedExposureTexture = ResourceDescriptorHeap[Constants.adaptedExposureBufferIndex];
    RWTexture2D<float> finalExposureTexture = ResourceDescriptorHeap[Constants.finalExposureTextureIndex];

    float raw = exposureTexture.Load(int3(0, 0, 0));
    raw = clamp(raw, Constants.clampMin, Constants.clampMax);
    if (!isfinite(raw) || raw <= 0.0f)
    {
        raw = 1.0f;
    }

    float prev = adaptedExposureTexture[uint2(0, 0)];
    if (Constants.resetHistory != 0u || !isfinite(prev) || prev <= 0.0f)
    {
        prev = raw;
    }
    prev = clamp(prev, Constants.clampMin, Constants.clampMax);

    // Inverse-exposure convention:
    // - raw > prev means scene got brighter (image should darken): use tauBright.
    // - raw < prev means scene got darker (image should brighten): use tauDark.
    bool sceneGotBrighter = (raw > prev);
    float tau = sceneGotBrighter ? Constants.tauBright : Constants.tauDark;

    float dt = max(Constants.dt, 0.0f);
    float k = 1.0f - exp(-dt / max(tau, 1e-3f));
    k = saturate(k);

    float next = lerp(prev, raw, k);
    next = clamp(next, Constants.clampMin, Constants.clampMax);

    adaptedExposureTexture[uint2(0, 0)] = next;
    const float finalExposure = rcp(max(next, 1e-6f)) * exp2(Constants.exposureCompensationEV);
    finalExposureTexture[uint2(0, 0)] = max(finalExposure, 1e-6f);
}

