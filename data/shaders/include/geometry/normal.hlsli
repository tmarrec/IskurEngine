// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

float2 SignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0,
                  v.y >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeNormal(float3 N)
{
    N /= (abs(N.x) + abs(N.y) + abs(N.z));
    float2 e = N.xy;
    if (N.z < 0.0)
    {
        e = (1.0 - abs(e.yx)) * SignNotZero(e.xy);
    }
    return e;
}

float3 DecodeNormal(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return normalize(n);
}

float2 DecodeSnorm16x2(uint packed)
{
    int2 q = int2((int)(packed << 16) >> 16, (int)packed >> 16);
    return max((float2)q / 32767.0f, -1.0f.xx);
}

float3 DecodePackedNormalOct(uint packed)
{
    return DecodeNormal(DecodeSnorm16x2(packed));
}

float2 DecodePackedHalf2(uint packed)
{
    uint lo = packed & 0xFFFFu;
    uint hi = packed >> 16;
    return float2(f16tof32(lo), f16tof32(hi));
}

float4 DecodePackedColorRGBA16UNORM(uint packedLo, uint packedHi)
{
    const float kInvUnorm16 = 1.0f / 65535.0f;
    float4 c;
    c.x = (packedLo & 0xFFFFu) * kInvUnorm16;
    c.y = ((packedLo >> 16) & 0xFFFFu) * kInvUnorm16;
    c.z = (packedHi & 0xFFFFu) * kInvUnorm16;
    c.w = ((packedHi >> 16) & 0xFFFFu) * kInvUnorm16;
    return c;
}

float3 DecodePackedTangentR10G10B10A2(uint packed, out float handedness)
{
    uint x = packed & 1023u;
    uint y = (packed >> 10) & 1023u;
    uint z = (packed >> 20) & 1023u;
    uint a = (packed >> 30) & 3u;

    float3 t = float3(x, y, z) * (1.0f / 1023.0f);
    t = t * 2.0f - 1.0f;
    handedness = (a >= 2u) ? 1.0f : -1.0f;
    return normalize(t);
}

