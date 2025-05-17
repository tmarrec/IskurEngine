// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define PI 3.14159265358979323846

struct GlobalConstants
{
    float4x4 proj;
    float4x4 view;

    float3 cameraPos;
    uint unused;

    float4 planes[6];

    uint raytracingOutputIndex;
    float3 sunDir;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

struct Meshlet
{
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

struct MeshletBounds
{
	/* bounding sphere, useful for frustum and occlusion culling */
    float3 center;
    float radius;

	/* normal cone, useful for backface culling */
    float3 cone_apex;
    float3 cone_axis;
    float cone_cutoff; /* = cos(angle/2) */
    
    float coneAxisAndCutoff;
};

struct VertexOut
{
    float3 posWorld : POSITION0;
    float3 posWorldView : POSITION1;
    float4 posWorldViewProj : SV_Position;
    float3 normal : NORMAL0;
    uint meshletIndex : COLOR0;
    float2 texCoord : TEXCOORD0;
    float3x3 TBN : TBN;
};

struct Payload
{
    uint meshletIndices[32];
};

enum AlphaMode
{
    AlphaMode_Opaque = 0,
    AlphaMode_Blend = 1,
    AlphaMode_Mask = 2
};

struct Material
{
    float metallicFactor;
    float roughnessFactor;
    int baseColorTextureIndex;
    int baseColorSamplerIndex;

    float4 baseColorFactor;

    uint alphaMode;
    float alphaCutoff;
    int metallicRoughnessTextureIndex;
    int metallicRoughnessSamplerIndex;

    int normalTextureIndex;
    int normalSamplerIndex;
    float normalScale;
    int pad;
};

struct RootConstants
{
    float4x4 world;
    uint meshletCount;
    uint materialIdx;

    uint verticesBufferIndex;
    uint meshletsBufferIndex;
    uint meshletVerticesBufferIndex;
    uint meshletTrianglesBufferIndex;
    uint meshletBoundsBufferIndex;
    uint materialsBufferIndex;
};
ConstantBuffer<RootConstants> PrimitiveConstants : register(b0);
ConstantBuffer<GlobalConstants> Globals : register(b1);
