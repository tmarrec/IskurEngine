// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<ExposureConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=10, b0)"

[RootSignature(ROOT_SIG)]
[numthreads(1, 1, 1)]
void main()
{
    StructuredBuffer<uint> histogramBuffer = ResourceDescriptorHeap[Constants.histogramBufferIndex];
    RWTexture2D<float> exposureTexture = ResourceDescriptorHeap[Constants.exposureBufferIndex];

    uint total = 0u;
    for (uint i = 0u; i < Constants.numBuckets; ++i)
    {
        total += histogramBuffer[i];
    }

    if (total == 0u)
    {
        float prev = exposureTexture[uint2(0, 0)];
        if (!isfinite(prev) || prev <= 0.0f)
        {
            prev = 1.0f;
        }
        exposureTexture[uint2(0, 0)] = prev;
        return;
    }

    float t = saturate(Constants.targetPct);
    float lo = saturate(Constants.lowReject);
    float hi = saturate(Constants.highReject);
    if (hi < lo)
    {
        float tmp = hi;
        hi = lo;
        lo = tmp;
    }

    uint totalMinusOne = total - 1u;
    uint loRank = (uint)round(lo * totalMinusOne);
    uint hiRank = (uint)round(hi * totalMinusOne);
    uint targetRank = (uint)round(lerp((float)loRank, (float)hiRank, t));

    uint sum = 0u;
    uint chosenBin = Constants.numBuckets - 1u;
    for (uint i = 0u; i < Constants.numBuckets; ++i)
    {
        sum += histogramBuffer[i];
        if (sum > targetRank)
        {
            chosenBin = i;
            break;
        }
    }

    float binNorm = (Constants.numBuckets > 1u) ? ((float(chosenBin) + 0.5f) / float(Constants.numBuckets)) : 0.5f;
    float logLum = lerp(Constants.minLogLum, Constants.maxLogLum, saturate(binNorm));
    float Lp = exp2(logLum);

    float exposure = (Lp > 1e-6f) ? (Constants.key / Lp) : 1.0f;
    // Store inverse exposure for adaptation/clamping history.
    exposure = rcp(max(exposure, 1e-6f));
    exposureTexture[uint2(0, 0)] = exposure;
}

