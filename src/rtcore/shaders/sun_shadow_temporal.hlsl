// Phases 3 and 4: ray-traced sun shadows and ambient occlusion, between the trace and the filter: the temporal
// accumulation. Each pixel's surface is reprojected into the previous frame answered (its camera, and the render
// offset's change since); where that frame saw the same surface there (its depth agrees), this frame's traced
// visibility (sun and ambient) is blended into the running averages kept there, over up to `prev.w` frames. So a few
// rays a frame converge to smooth penumbrae and occlusion.
//
// Moving objects are reprojected as if they stood still: the depth test drops most of what that gets wrong on the
// objects themselves, and the history is bounded by the range this frame's rays found around the pixel (with a
// margin for how few they are), so that a shadow which moved on does not linger where everything is now lit (or
// appear late where everything is now in shadow).
//
// history: x the accumulated sun visibility, y the accumulated ambient visibility, z the number of frames in them.
// counts[] (shared with the filter, which clears them): 4 history kept, 5 history bounded by the neighbourhood.

Texture2D<float2> traced : register(t0);      // this frame's visibility, sun and ambient (the trace pass)
Texture2D<float> depth : register(t1);        // this frame's depth (the game's, as copied)
Texture2D<float> prev_depth : register(t2);   // the previous frame's
Texture2D<float4> prev_history : register(t3);
RWTexture2D<float4> history : register(u0);
RWByteAddressBuffer counts : register(u1);

cbuffer Frame : register(b0) {
    float4 cam_right;
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;          // P00, P11, P22, P32
    float4 range;         // x: the main scene's depth range end
    float4 sun;
    uint2 size;
    float tolerance;
    uint flags;           // 2: reversed depth; 8: no history (the first frame, new frame textures, History=1)
    uint4 shadow;         // x: rays a pixel, y: tan of the sun's radius, z: filter radius, w: frame
    float4 reproject[4];  // render-offset space now -> the previous frame's clip space (row-vector: clip = (p, 1) M)
    float4 prev;          // the previous frame's P22, P32, depth range end; w: max frames in the history
};

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

void count(uint slot, bool when) {
    const uint k = WaveActiveCountBits(when);
    if (k && WaveIsFirstLane()) counts.InterlockedAdd(slot * 4, k);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const float2 v = traced[id.xy];
    const float d = depth[id.xy] / range.x;
    const bool reversed = (flags & 2) != 0;
    const bool surface = !(reversed ? d <= 0.0 : d >= 1.0);
    float4 h = float4(v, 1.0, 0.0);
    bool kept = false, bounded = false;
    if (surface && !(flags & 8)) {
        const float z = proj.w / (d - proj.z);
        const float3 p = cam_origin.xyz + view_ray(int2(id.xy)) * z;
        // where it was: the previous frame's clip space
        const float4 c = p.x * reproject[0] + p.y * reproject[1] + p.z * reproject[2] + reproject[3];
        if (c.w > 1e-3) {
            const float2 ndc = c.xy / c.w;
            const float2 pix = float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
            if (all(pix >= 0.0) && all(pix < float2(size))) {
                const uint2 q = uint2(pix);
                const float pd = prev_depth[q] / prev.z;
                const bool prev_sky = reversed ? pd <= 0.0 : pd >= 1.0;
                const float pz = prev_sky ? 1e9 : prev.y / (pd - prev.x);
                // the same surface: its view depth then (c.w) against what the previous frame's depth says there
                if (abs(pz - c.w) <= max(c.w * 0.03, 0.05)) {
                    float2 lo = v, hi = v;
                    for (int y = -1; y <= 1; ++y) {
                        for (int x = -1; x <= 1; ++x) {
                            const float2 t = traced[uint2(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1))];
                            lo = min(lo, t);
                            hi = max(hi, t);
                        }
                    }
                    const float4 old = prev_history[q];
                    const float2 o = clamp(old.xy, lo - 0.15, hi + 0.15);
                    bounded = any(abs(o - old.xy) > 0.01);
                    const float n = min(old.z + 1.0, prev.w);
                    h = float4(lerp(o, v, 1.0 / n), n, 0.0);
                    kept = true;
                }
            }
        }
    }
    history[id.xy] = h;
    count(4, kept);
    count(5, bounded);
}
