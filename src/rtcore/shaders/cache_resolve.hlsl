// The world radiance cache (radiance_cache.hlsli), pass 2 of 2: each cell seen this frame takes the mean of its samples
// into its value (a running average over about 8 frames while it stays in view; anew when it comes back after a
// second); a cell not seen for half a minute is freed.

#include "trace_common.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint s = id.x;
    if (s >= kCacheSlots || cache_keys[s] == 0u) return;
    const uint n = cache_accum[s * 4u + 3u];
    const uint now = shadow.w + 1u;
    float4 v = cache_value[s];
    const uint last = asuint(v.a);
    if (n) {
        const float3 mean = float3(cache_accum[s * 4u], cache_accum[s * 4u + 1u], cache_accum[s * 4u + 2u]) / (256.0 * float(n));
        v.rgb = (last != 0u && now - last < 60u) ? lerp(v.rgb, mean, 0.125) : mean;
        v.a = asfloat(now);
        cache_value[s] = v;
        cache_accum[s * 4u + 0u] = 0u;
        cache_accum[s * 4u + 1u] = 0u;
        cache_accum[s * 4u + 2u] = 0u;
        cache_accum[s * 4u + 3u] = 0u;
    } else if (last != 0u && now - last > 1800u) {
        cache_keys[s] = 0u;
        cache_value[s] = float4(0.0, 0.0, 0.0, 0.0);
    }
}
