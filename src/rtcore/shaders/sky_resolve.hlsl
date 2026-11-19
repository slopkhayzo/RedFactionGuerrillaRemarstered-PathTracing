// The drawn sky (sky_probe.hlsli), pass 2 of 2, one group: each texel that got samples this frame moves toward their
// mean (over about 10 frames; its first samples at once); a texel not seen for half a minute is forgotten; then the mean
// of the texels seen goes to the slot after them, for the directions not seen.

#include "trace_common.hlsli"

groupshared float4 gs_sum[1024];
RWStructuredBuffer<uint> frame_counts : register(u6);  // the frame's counters: the map's mean for the log, from uint 48

[numthreads(1024, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint t = id.x;
    float4 v = sky_value[t];
    const uint n = sky_accum[t * 4u + 3u];
    const uint now = shadow.w + 1u;
    if (n) {
        const float3 mean = float3(sky_accum[t * 4u], sky_accum[t * 4u + 1u], sky_accum[t * 4u + 2u]) / (256.0 * float(n));
        v = float4(asuint(v.a) != 0u ? lerp(v.rgb, mean, 0.1) : mean, asfloat(now));
        sky_value[t] = v;
        sky_accum[t * 4u + 0u] = 0u;
        sky_accum[t * 4u + 1u] = 0u;
        sky_accum[t * 4u + 2u] = 0u;
        sky_accum[t * 4u + 3u] = 0u;
    } else if (asuint(v.a) != 0u && now - asuint(v.a) > 1800u) {
        v = float4(0.0, 0.0, 0.0, 0.0);  // not seen for half a minute
        sky_value[t] = v;
    }
    gs_sum[t] = asuint(v.a) != 0u ? float4(v.rgb, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 512u; s > 0u; s >>= 1) {
        if (t < s) gs_sum[t] += gs_sum[t + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u) {
        sky_value[kSkyTexels] = gs_sum[0].w > 0.0 ? float4(gs_sum[0].rgb / gs_sum[0].w, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
        const float3 m = gs_sum[0].w > 0.0 ? gs_sum[0].rgb / gs_sum[0].w : float3(0.0, 0.0, 0.0);
        frame_counts[48] = asuint(m.r);
        frame_counts[49] = asuint(m.g);
        frame_counts[50] = asuint(m.b);
        frame_counts[51] = uint(gs_sum[0].w);
    }
}
