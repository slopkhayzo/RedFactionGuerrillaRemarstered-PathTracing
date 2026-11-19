// Global illumination, pass 3 of 3 with GI.Wavelet: an edge-avoiding a-trous wavelet filter (as SVGF's), in place of
// gi_filter.hlsl's single pass. Four passes of a 5x5 B3-spline kernel at steps of 1, 2, 4 and 8 pixels (about 61 x 61
// pixels in all), each neighbour weighed
//   - by how near it lies to the pixel's surface plane (the normal from the depth): edges and creases stay put;
//   - by how near its irradiance is to the pixel's, against the noise left around the pixel: the first pass measures it
//     (the 3x3 spread of the accumulated irradiance's luminance) and the passes after carry it in alpha. Where the
//     history is long and the noise low, only like values mix (the light's own detail, the occlusion in corners, stays);
//     where it is short and noisy, the filter reaches further.
// The irradiance has no texture in it (the patched shaders multiply it by their albedo): only the light is smoothed.
// The last pass writes the irradiance texture the patched lit shaders read (alpha 1: traced; 0 for the sky and the
// landmark layer, where they keep the game's own ambient).

Texture2D<float4> input : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<float4> output : register(u0);

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
    uint flags;           // 2: reversed depth
    uint4 shadow;
    float4 reproject[4];
    float4 prev;
    float4 ambient;
    float4 back_ambient;
    float4 sun_color;
    float4 gi;
};

cbuffer Pass : register(b1) {
    uint step;  // 1, 2, 4, 8
    uint last;  // 1: the last pass, into the irradiance texture
};

static const float kKernel[3] = {0.375, 0.25, 0.0625};

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

// View depth (m) at a pixel; -1 for the sky, the landmark layer and outside the frame.
float view_depth(int2 p) {
    if (any(p < 0) || any(p >= int2(size))) return -1.0;
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return -1.0;
    return proj.w / (d - proj.z);
}

float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    const float z0 = view_depth(px);
    if (z0 < 0.0) {
        output[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float3 p0 = view_ray(px) * z0;  // camera-relative
    // the surface's normal from the neighbouring depths, the side with the smaller step (as trace_common's surface_at)
    float3 n0;
    {
        const float zl = view_depth(px + int2(-1, 0)), zr = view_depth(px + int2(1, 0));
        const float zu = view_depth(px + int2(0, -1)), zd = view_depth(px + int2(0, 1));
        const float3 l = view_ray(px + int2(-1, 0)) * zl, r = view_ray(px + int2(1, 0)) * zr;
        const float3 u = view_ray(px + int2(0, -1)) * zu, d = view_ray(px + int2(0, 1)) * zd;
        const float3 dx = (zr > 0.0 && (zl < 0.0 || abs(zr - z0) < abs(z0 - zl))) ? r - p0 : (zl > 0.0 ? p0 - l : float3(0, 0, 0));
        const float3 dy = (zd > 0.0 && (zu < 0.0 || abs(zd - z0) < abs(z0 - zu))) ? d - p0 : (zu > 0.0 ? p0 - u : float3(0, 0, 0));
        const float3 c = cross(dy, dx);
        n0 = dot(c, c) > 1e-12 ? normalize(c) : -normalize(p0);
    }
    const float4 c0 = input[id.xy];
    const float l0 = lum(c0.rgb);
    // the noise around the pixel: measured in the first pass, carried in alpha after it
    float sigma = c0.a;
    if (step <= 1u) {
        float m1 = 0.0, m2 = 0.0, k = 0.0;
        for (int sy = -1; sy <= 1; ++sy) {
            for (int sx = -1; sx <= 1; ++sx) {
                const int2 q = px + int2(sx, sy);
                if (view_depth(q) < 0.0) continue;
                const float l = lum(input[uint2(q)].rgb);
                m1 += l;
                m2 += l * l;
                k += 1.0;
            }
        }
        const float mu = m1 / max(k, 1.0);
        sigma = sqrt(max(m2 / max(k, 1.0) - mu * mu, 0.0));
    }
    // a neighbour this far off the pixel's plane (m) weighs 1/e: wider at a distance and at the wider steps
    const float plane_tol = 0.01 * z0 * float(step) + 0.02;
    float3 sum = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const int2 q = px + int2(x, y) * int(step);
            const float z = view_depth(q);
            if (z < 0.0) continue;
            const float3 pq = view_ray(q) * z;
            const float wp = exp(-abs(dot(n0, pq - p0)) / plane_tol);
            const float4 c = input[uint2(q)];
            const float wl = exp(-abs(lum(c.rgb) - l0) / (4.0 * sigma + 1e-4));
            const float w = kKernel[abs(x)] * kKernel[abs(y)] * wp * wl;
            sum += c.rgb * w;
            wsum += w;
        }
    }
    const float3 res = wsum > 0.0 ? sum / wsum : c0.rgb;
    output[id.xy] = float4(res, last != 0u ? 1.0 : sigma);
}
