// Global illumination, pass 2 of 3: the irradiance accumulated over frames, as the shadows' (sun_shadow_temporal.hlsl):
// each pixel's surface reprojected into the previous frame answered, its history kept where that frame's depth agrees,
// this frame's traced irradiance blended into it, over up to gi.w frames, fewer where the light changed (below). No
// per-pixel neighbourhood bound: with a few noisy rays a pixel it would hold the noise; moved surfaces fail the depth
// test.
//
// history: rgb the accumulated irradiance, a the number of frames in it.

Texture2D<float4> traced : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float> prev_depth : register(t2);
Texture2D<float4> prev_history : register(t3);
RWTexture2D<float4> history : register(u0);

cbuffer Frame : register(b0) {
    float4 cam_right;
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;
    float4 range;
    float4 sun;
    uint2 size;
    float tolerance;
    uint flags;           // 2: reversed depth; 8: no history
    uint4 shadow;
    float4 reproject[4];  // render-offset space now -> the previous frame's clip space (row-vector)
    float4 prev;          // the previous frame's P22, P32, depth range end
    float4 ambient;
    float4 back_ambient;
    float4 sun_color;
    float4 gi;            // w: max frames averaged
};

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const float3 v = traced[id.xy].rgb;
    const float d = depth[id.xy] / range.x;
    const bool reversed = (flags & 2) != 0;
    const bool surface = !(reversed ? d <= 0.0 : d >= 1.0);
    float4 h = float4(v, 1.0);
    if (surface && !(flags & 8)) {
        const float z = proj.w / (d - proj.z);
        const float3 p = cam_origin.xyz + view_ray(int2(id.xy)) * z;
        const float4 c = p.x * reproject[0] + p.y * reproject[1] + p.z * reproject[2] + reproject[3];
        if (c.w > 1e-3) {
            const float2 ndc = c.xy / c.w;
            const float2 pix = float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
            if (all(pix >= 0.0) && all(pix < float2(size))) {
                const uint2 q = uint2(pix);
                const float pd = prev_depth[q] / prev.z;
                const bool prev_sky = reversed ? pd <= 0.0 : pd >= 1.0;
                const float pz = prev_sky ? 1e9 : prev.y / (pd - prev.x);
                if (abs(pz - c.w) <= max(c.w * 0.03, 0.05)) {
                    const float4 old = prev_history[q];
                    // Where the light changed (something moved past, a light came on or went out), the history is cut
                    // short, so the change shows within a few frames rather than fading in over gi.w: the history against
                    // this frame's light around the pixel, a 5x5 mean of the traced values. A few rays a pixel make that
                    // mean noisy: a difference counts only where it is large (30 %, 50 %) and also beyond three
                    // standard errors of the mean (the 25 values' spread), else the noise alone cut the history short
                    // in the shade and kept it grainy.
                    const float3 lw = float3(0.2126, 0.7152, 0.0722);
                    float3 mean = 0.0;
                    float k = 0.0, m2 = 0.0;
                    for (int dy = -2; dy <= 2; ++dy) {
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int2 s = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(size) - 1);
                            const float4 t = traced[uint2(s)];
                            if (t.a > 0.0) {
                                mean += t.rgb;
                                const float l = dot(t.rgb, lw);
                                m2 += l * l;
                                k += 1.0;
                            }
                        }
                    }
                    float cap = max(gi.w, 1.0);
                    if (k > 0.0) {
                        const float a = dot(old.rgb, lw), b = dot(mean / k, lw);
                        const float se = sqrt(max(m2 / k - b * b, 0.0) / k);
                        const float change = abs(a - b) / (max(a, b) + 1e-3);
                        if (abs(a - b) > 3.0 * se) {
                            if (change > 0.5)
                                cap = min(cap, 3.0);
                            else if (change > 0.3)
                                cap = min(cap, 6.0);
                        }
                    }
                    const float n = min(old.a + 1.0, cap);
                    h = float4(lerp(old.rgb, v, 1.0 / n), n);
                }
            }
        }
    }
    history[id.xy] = h;
}
