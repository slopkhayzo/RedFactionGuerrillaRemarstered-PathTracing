// The drawn sky (sky_probe.hlsli), pass 1 of 2: a sixteenth of the pixels a frame (each 4x4 block's in turn) that show
// the sky in this frame and the previous one add the previous frame's colour there, over pi, to their direction's texel;
// not the sun's disk and glow (within sky_probe.y's cosine of its direction), not below the horizon.

#include "trace_common.hlsli"

RWStructuredBuffer<uint> frame_counts : register(u6);  // the frame's counters: a sample for the log, from uint 52

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (all(id.xy == 0u)) {  // what the pass reads at the top of the screen, its centre (sky, mostly)
        const uint2 c = uint2(size.x / 2u, 2u);
        const float4 r = radiance[c];
        frame_counts[52] = asuint(r.r);
        frame_counts[53] = asuint(r.g);
        frame_counts[54] = asuint(r.b);
        frame_counts[55] = asuint(depth[c]);
        frame_counts[56] = asuint(prev_depth[c]);
        frame_counts[57] = flags;
    }
    const uint k = shadow.w & 15u;
    const int2 px = int2(id.xy * 4u + uint2(k & 3u, k >> 2));
    if (any(px >= int2(size)) || (flags & 16u) == 0u) return;
    const bool reversed = (flags & 2u) != 0u;
    const float d = depth[uint2(px)], pd = prev_depth[uint2(px)];
    if (reversed ? (d > 0.0 || pd > 0.0) : (d < 1.0 || pd < 1.0)) return;  // a surface, or the landmark layer
    const float3 dir = normalize(view_ray(px));
    if (dir.y < 0.0) return;
    if (sun.w != 0.0 && dot(dir, normalize(sun.xyz)) > sky_probe.y) return;
    const float3 lit = radiance[uint2(px)].rgb;
    // The game clears its scene to pure green (0, 1, 0) and shows that where its sky is not drawn yet, its first seconds:
    // learnt, it lit everything green (and the directions not looked at again stayed so).
    if (lit.g > 0.5 && lit.r < 0.01 && lit.b < 0.01) return;
    const float3 L = max(lit, 0.0) / PI;
    const uint t = sky_texel(dir);
    const uint3 q = uint3(min(L, 255.0) * 256.0 + 0.5);
    InterlockedAdd(sky_accum[t * 4u + 0u], q.x);
    InterlockedAdd(sky_accum[t * 4u + 1u], q.y);
    InterlockedAdd(sky_accum[t * 4u + 2u], q.z);
    InterlockedAdd(sky_accum[t * 4u + 3u], 1u);
}
