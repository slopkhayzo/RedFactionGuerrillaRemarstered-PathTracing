#pragma once
// The world radiance cache: the light leaving the surfaces the game drew, kept by place and facing in world space, so
// that rays which hit them later, off screen, still get it (and the light they bring back goes on bouncing: a bounce
// more a frame). A hash grid as NVIDIA's SHaRC keeps one: cells whose size grows with the distance from the camera
// (cache_level: about 1.5 % of it, from 12.5 cm, in powers of two), one per facing (the normal's major axis and sign),
// in buckets of 8 slots that a lookup probes whole (a slot freed in the middle of one breaks nothing).
//
// cache_update.hlsl fills it from the screen (the game's lit colour of the previous frame where it saw the surface, as
// seen_radiance reads it), cache_resolve.hlsl averages each cell's samples into its value and frees the cells not seen
// for a while; shade_hit and shade_hit2 (trace_common.hlsli) read it for the hits the previous frame did not show.
RWStructuredBuffer<uint> cache_keys : register(u1);     // 0: a free slot
RWStructuredBuffer<uint> cache_accum : register(u2);    // this frame's samples, 4 a slot: r, g, b sums (1/256 units), count
RWStructuredBuffer<float4> cache_value : register(u3);  // rgb: the radiance (the game's units over pi); a: the frame it
                                                        // was last seen, plus one (asuint; 0: never)

static const uint kCacheSlots = 1u << 20;
static const uint kCacheBucket = 8u;

uint cache_hash(uint x) {  // lowbias32
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Level l: cells of 0.125 * 2^l m; about 1.5 % of the distance from the camera.
uint cache_level(float dist) { return min(uint(max(log2(max(dist * 0.015, 0.125) / 0.125), 0.0)), 15u); }

uint cache_facing(float3 n) {
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.z) return n.y >= 0.0 ? 2u : 3u;
    return n.z >= 0.0 ? 4u : 5u;
}

// A world position's cell (render-offset space + world.xyz), its normal and its distance from the camera -> its bucket's
// first slot and its key (two hashes of the same cell, from different seeds: a cell is taken for another only if both
// agree).
void cache_cell(float3 p, float3 n, float dist, out uint base, out uint key) {
    const uint level = cache_level(dist);
    const int3 c = int3(floor((p + world.xyz) / (0.125 * exp2(float(level)))));
    const uint tag = (level << 3) | cache_facing(n);
    uint a = cache_hash(uint(c.x) + 0x9E3779B9u);
    a = cache_hash(a ^ uint(c.y));
    a = cache_hash(a ^ uint(c.z));
    a = cache_hash(a ^ tag);
    uint b = cache_hash(uint(c.x) ^ 0x85EBCA6Bu);
    b = cache_hash(b + uint(c.y));
    b = cache_hash(b + uint(c.z));
    b = cache_hash(b + tag);
    base = (a & (kCacheSlots / kCacheBucket - 1u)) * kCacheBucket;
    key = b ? b : 1u;
}

// The radiance the cache holds for a surface point (its normal facing where the ray came from); false if none.
bool cache_radiance(float3 p, float3 n, out float3 L) {
    L = 0;
    if (world.w == 0.0) return false;
    uint base, key;
    cache_cell(p, n, length(p - cam_origin.xyz), base, key);
    for (uint i = 0; i < kCacheBucket; ++i) {
        if (cache_keys[base + i] != key) continue;
        const float4 v = cache_value[base + i];
        if (asuint(v.a) == 0u) return false;  // made this frame, not averaged yet
        L = v.rgb;
        return true;
    }
    return false;
}
