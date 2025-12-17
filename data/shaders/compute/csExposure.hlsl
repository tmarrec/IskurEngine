// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<ExposureConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=10, b0)"

[RootSignature(ROOT_SIG)]
[NumThreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint3 gtid: SV_GroupThreadID)
{
    StructuredBuffer<uint> histogramBuffer = ResourceDescriptorHeap[Constants.histogramBufferIndex];

    float t = saturate(Constants.targetPct);
    float lo = saturate(Constants.lowReject);
    float hi = saturate(Constants.highReject);
    if (hi < lo)
    {
        float tmp = hi;
        hi = lo;
        lo = tmp;
    }

    float tTrim = lerp(lo, hi, t);
    uint totalMinusOne = (Constants.totalPixels > 0u) ? (Constants.totalPixels - 1u) : 0u;
    uint targetRank = (uint)round(tTrim * totalMinusOne);

    uint sum = 0u;
    uint chosenBin = 0u;
    for (uint i = 0u; i < Constants.numBuckets; ++i)
    {
        sum += histogramBuffer[i];
        if (sum > targetRank)
        {
            chosenBin = i;
            break;
        }
    }

    float binNorm = (Constants.numBuckets > 1u) ? (float(chosenBin) / float(Constants.numBuckets - 1u)) : 0.0f;
    float logLum = lerp(Constants.minLogLum, Constants.maxLogLum, binNorm);
    float Lp = exp2(logLum);

    float exposure = (Lp > 1e-6f) ? (Constants.key / Lp) : 1.0f;

    RWStructuredBuffer<float> exposureBuffer = ResourceDescriptorHeap[Constants.exposureBufferIndex];
    exposureBuffer[0] = exposure;
}
