// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexID : SV_VertexID)
{
    // Fullscreen triangle verts in clip space
    float2 verts[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOut o;
    o.position = float4(verts[vertexID], 0.0, 1.0);

    float2 uv = o.position.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y; // D3D
    o.uv = uv;
    return o;
}