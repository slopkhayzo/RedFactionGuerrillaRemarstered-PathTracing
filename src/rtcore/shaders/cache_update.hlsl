// The world radiance cache (radiance_cache.hlsli), pass 1 of 2: a quarter of the pixels a frame (each 2x2 block's
// pixels in turn) add the light the game drew for their surface in the previous frame (seen_radiance: reprojected,
// where the depths agree) to their cell, made in its bucket's first free slot when it has none yet.

#include "trace_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint k = shadow.w & 3u;
    const int2 px = int2(id.xy * 2u + uint2(k & 1u, k >> 1));
    if (any(px >= int2(size))) return;
    float3 p, n;
    if (!surface_at(px, p, n)) return;
    float3 L;
    if (!seen_radiance(p, L)) return;
    uint base, key;
    cache_cell(p, n, length(p - cam_origin.xyz), base, key);
    uint slot = 0xFFFFFFFFu;
    for (uint i = 0; i < kCacheBucket && slot == 0xFFFFFFFFu; ++i)
        if (cache_keys[base + i] == key) slot = base + i;
    for (uint j = 0; j < kCacheBucket && slot == 0xFFFFFFFFu; ++j) {
        uint prev;
        InterlockedCompareExchange(cache_keys[base + j], 0u, key, prev);
        if (prev == 0u || prev == key) slot = base + j;
    }
    if (slot == 0xFFFFFFFFu) return;  // the bucket is full
    const uint3 q = uint3(min(L, 255.0) * 256.0 + 0.5);
    InterlockedAdd(cache_accum[slot * 4u + 0u], q.x);
    InterlockedAdd(cache_accum[slot * 4u + 1u], q.y);
    InterlockedAdd(cache_accum[slot * 4u + 2u], q.z);
    InterlockedAdd(cache_accum[slot * 4u + 3u], 1u);
}
