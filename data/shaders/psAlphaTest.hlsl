// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/Common.hlsli"
#include "data/CPUGPU.h"

ConstantBuffer<PrimitiveConstants> Constants : register(b0);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=40, b0), \
                  CBV(b1)"

[RootSignature(ROOT_SIG)]
void main(VertexOut input)
{
    StructuredBuffer<Material> materialsBuffer = ResourceDescriptorHeap[Constants.materialsBufferIndex];
    Material material = materialsBuffer[Constants.materialIdx];

    // Color
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSampler = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= baseColorTexture.Sample(baseColorSampler, input.texCoord.xy);
    }

    // Alpha
    float alpha = baseColor.a;
    clip(alpha - material.alphaCutoff);
}