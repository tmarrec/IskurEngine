// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "data/shaders/common.hlsli"

bool IsVisible(MeshletBounds bounds, float4x4 world, float scale, float3 viewPos)
{
    // Frustum culling
    float4 center = mul(float4(bounds.center, 1), world);
    float radius = bounds.radius * scale;
    for (int i = 0; i < 6; ++i)
    {
        if (dot(center, Globals.planes[i]) < -radius)
        {
            return false;
        }
    }
    
    /*
    // Backface culling
    if (dot(normalize(bounds.cone_apex - viewPos), bounds.cone_axis) >= bounds.cone_cutoff)
    {
        return false;
    }
    */
    
    return true;
}