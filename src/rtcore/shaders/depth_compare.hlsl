// Phase 2 step 4 (research/docs/03): the mirror's depth against the game's. One ray per pixel from the game's camera
// into the frame's TLAS; where it hits is compared with the game's depth at that pixel.
//
// The result goes into ClbTexture: G (the sun-shadow channel) is 1 where the two agree (or both see sky) and 0 where
// they don't, so that every mismatch shows as shadow in the game's own image; R (the ambient-occlusion channel) shows
// the mirror's own depth as bands (log distance; 1 where its rays miss), so that its geometry can be seen against
// the game's. Per pixel class, counts[]:
//   0 match, 1 sky (both), 2 missing (the game has a surface, the mirror nothing), 3 in front (the mirror has a surface
//   closer than the game's, or where the game sees sky), 4 behind (the mirror's surface is farther: something the game
//   draws is missing in front of it); counts[8 + i]: where both have a surface, a histogram of log2(mirror depth /
//   game depth) in 16 buckets of 1/4 from -2 to +2 (a peak away from the middle is a scale, not missing geometry).
// Split view: the left half of R shows the game's own depth with the same bands, for comparison.
// counts[32 + 3 i]: 32 sample pixels (an 8 x 4 grid of cell centres): the game's depth value, the mirror's view depth
// (-1: nothing hit), the game's view depth as linearized here.

#include "rt_query.hlsli"
RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<unorm float2> result : register(u0);
RWByteAddressBuffer counts : register(u1);

cbuffer Frame : register(b0) {
    float4 cam_right;    // xyz: camera axes and position (render-offset space)
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;         // P00, P11, P22, P32 of the game's projection (row-vector: clip = (x, y, z, 1) P)
    float4 range;        // x: the main scene's depth range end (depth values are the projection's times this)
    float4 sun;          // unused here; the layout is the core's CompareConstants
    uint2 size;
    float tolerance;     // relative depth difference still counted as a match
    uint flags;          // bit 0: clear the counters only; bit 1: reversed depth (near 1, far 0)
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (flags & 1) {
        if (all(id == 0))
            for (uint i = 0; i < 128; ++i) counts.Store(i * 4, 0);
        return;
    }
    if (any(id.xy >= size)) return;
    // The game's depth, back to the projection's own range; beyond the main scene's range lie the landmark LOD layer
    // and the sky, which the mirror leaves out: counted as sky.
    const float d_raw = depth[id.xy];
    const float d = d_raw / range.x;
    const bool reversed = (flags & 2) != 0;
    const bool game_sky = reversed ? d <= 0.0 : d >= 1.0;
    const float2 ndc = float2((id.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / size.y * 2.0);
    const float3 v = float3(ndc.x / proj.x, ndc.y / proj.y, 1.0);  // view space, z = 1
    const float3 dir = normalize(v.x * cam_right.xyz + v.y * cam_up.xyz + v.z * cam_forward.xyz);

    RayDesc ray;
    ray.Origin = cam_origin.xyz;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = 100000.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);  // instances drawn in the main pass
    RT_PROCEED(q);
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

    uint cls;
    int bucket = -1;
    float z_game = 0.0;
    if (!game_sky) z_game = proj.w / (d - proj.z);  // view depth from the depth buffer
    if (game_sky) {
        cls = hit ? 3 : 1;
    } else if (!hit) {
        cls = 2;
    } else {
        const float z_rt = q.CommittedRayT() * dot(dir, cam_forward.xyz);  // view depth of the hit
        const float rel = (z_rt - z_game) / max(z_game, 1e-3);
        cls = abs(rel) <= tolerance ? 0 : (rel < 0.0 ? 3 : 4);
        bucket = clamp(int(floor((log2(max(z_rt, 1e-3) / max(z_game, 1e-3)) + 2.0) * 4.0)), 0, 15);
    }
    // one atomic per class and wave, not per pixel
    for (uint c = 0; c < 5; ++c) {
        const uint k = WaveActiveCountBits(cls == c);
        if (k && WaveIsFirstLane()) counts.InterlockedAdd(c * 4, k);
    }
    for (int b = 0; b < 16; ++b) {
        const uint k = WaveActiveCountBits(bucket == b);
        if (k && WaveIsFirstLane()) counts.InterlockedAdd((8 + b) * 4, k);
    }
    const uint2 cell = uint2(id.x * 8 / size.x, id.y * 4 / size.y);
    if (all(id.xy == uint2((cell.x * 2 + 1) * size.x / 16, (cell.y * 2 + 1) * size.y / 8))) {
        const uint at = (32 + (cell.y * 8 + cell.x) * 3) * 4;
        counts.Store(at, asuint(d_raw));
        counts.Store(at + 4, asuint(hit ? q.CommittedRayT() * dot(dir, cam_forward.xyz) : -1.0));
        counts.Store(at + 8, asuint(z_game));
    }
    // the bands: the distance along the ray in R (0: nothing there), the mirror's on the right half, the game's own on
    // the left; G: whether the mirror agrees with the game (always 1 on the left half)
    float band = 0.0;
    const bool left = id.x < size.x / 2;
    if (!left) {
        if (hit) band = 0.3 + 0.7 * frac(log2(max(q.CommittedRayT(), 0.1)) * 2.0);
    } else if (!game_sky) {
        band = 0.3 + 0.7 * frac(log2(max(z_game / max(dot(dir, cam_forward.xyz), 1e-3), 0.1)) * 2.0);
    }
    result[id.xy] = float2(band, (left || cls <= 1) ? 1.0 : 0.0);
}
