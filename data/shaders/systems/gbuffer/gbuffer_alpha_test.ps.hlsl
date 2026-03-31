// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CPUGPU.h"

ConstantBuffer<PrimitiveConstants> Constants : register(b0);
ConstantBuffer<VertexConstants> VertexConstants : register(b1);

#define ROOT_SIG "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), \
                  RootConstants(num32BitConstants=60, b0), \
                  CBV(b1)"

struct VertexOut
{
    float4 clipPos : SV_Position;
    float4 color : TEXCOORD0;
    float2 texCoord : TEXCOORD1;
};

[RootSignature(ROOT_SIG)]
void main(VertexOut input)
{
    StructuredBuffer<Material> materialsBuffer = ResourceDescriptorHeap[Constants.materialsBufferIndex];
    Material material = materialsBuffer[Constants.materialIdx];

    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTextureIndex != -1)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSampler = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        baseColor *= baseColorTexture.SampleBias(baseColorSampler, input.texCoord.xy, VertexConstants.materialTextureMipBias);
    }
    baseColor *= input.color;

    float alpha = baseColor.a;
    clip(alpha - material.alphaCutoff);
}

