// Phases 3 and 4: ray-traced sun shadows and ambient occlusion, pass 3 of 3. Averages the visibility (traced, then
// accumulated over frames by pass 2) over a small neighbourhood, weighted by how close each neighbour's surface is to
// the pixel's own (view depth), so that penumbrae and occlusion smooth out and edges stay put; the result goes into
// ClbTexture's sun-shadow (G) and ambient-occlusion (R) channels, where the game's cascades and SSAO put theirs.
//
// counts[] compares the sun's with the game's own ClbTexture (its cascades, copied at the seam), thresholded at 1/2
// (flags 1 clears all of them, before the passes that add to them):
//   0 both lit, 1 both shadowed, 2 only ours shadowed, 3 only the game's shadowed, 8 sky (no surface)
// With Render.DebugView=1 (flags 4), R is the comparison display instead of the occlusion: the game's shadow on the
// left half, ours on the right.

Texture2D<float4> visibility : register(t0);  // the history: x sun, y ambient, accumulated over frames (pass 2)
Texture2D<float> depth : register(t1);
Texture2D<float2> game_clb : register(t2);
RWTexture2D<unorm float2> result : register(u0);
RWByteAddressBuffer counts : register(u1);

cbuffer Frame : register(b0) {
    float4 cam_right;
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;       // P00, P11, P22, P32
    float4 range;      // x: the main scene's depth range end
    float4 sun;
    uint2 size;
    float tolerance;
    uint flags;        // 1: clear the counters only; 2: reversed depth; 4: the debug display in R
    uint4 shadow;      // z: filter radius
};

float view_depth(int2 p) {
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return -1.0;
    return proj.w / (d - proj.z);
}

void count(uint slot, bool when) {
    const uint k = WaveActiveCountBits(when);
    if (k && WaveIsFirstLane()) counts.InterlockedAdd(slot * 4, k);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (flags & 1) {
        if (all(id == 0))
            for (uint i = 0; i < 128; ++i) counts.Store(i * 4, 0);
        return;
    }
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    const bool left = id.x < size.x / 2;
    const float game = game_clb[id.xy].y;
    const float z0 = view_depth(px);
    if (z0 < 0.0) {
        count(8, true);
        result[id.xy] = float2(1.0, 1.0);
        return;
    }
    const int r = int(shadow.z);
    float2 sum = 0.0;
    float wsum = 0.0;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int2 q = clamp(px + int2(x, y), int2(0, 0), int2(size) - 1);
            const float z = view_depth(q);
            if (z < 0.0) continue;
            // neighbours on another surface (a different depth) count little
            const float w = exp(-abs(z - z0) / max(z0 * 0.02, 0.02)) * exp(-float(x * x + y * y) / float(r * r + 1));
            sum += visibility[uint2(q)].xy * w;
            wsum += w;
        }
    }
    const float2 v = wsum > 0.0 ? sum / wsum : visibility[id.xy].xy;
    const bool lit = v.x > 0.5, game_lit = game > 0.5;
    count(0, lit && game_lit);
    count(1, !lit && !game_lit);
    count(2, !lit && game_lit);
    count(3, lit && !game_lit);
    const float shown = left ? game : v.x;
    result[id.xy] = float2((flags & 4) ? 0.05 + 0.95 * shown : v.y, v.x);
}
