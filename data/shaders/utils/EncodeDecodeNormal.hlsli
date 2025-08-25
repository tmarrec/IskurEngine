// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

float2 EncodeNormal(float3 N)
{
    N /= (abs(N.x) + abs(N.y) + abs(N.z));
    float2 e = N.xy;
    if (N.z < 0)
    {
        e = (1 - abs(e.yx)) * sign(e.xy);
    }
    return e;
}

float3 DecodeNormal(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}
