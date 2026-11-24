// DLSS Ray Reconstruction's guide images (dlss.h), each frame before it denoises the traced irradiance: per pixel
//   - the linear view depth (1e6 for the sky);
//   - the motion since the previous frame, in pixels (previous = current + motion): the surface reprojected into the
//     previous frame as the GI's temporal pass does (the camera's motion: the game gives no object motion), the sky by
//     its direction;
//   - the world normal from the depth, facing the camera, with roughness 1 in w (a diffuse signal);
//   - a white albedo (black for the sky): the irradiance has no texture in it, so none can be smoothed away.
// The specular albedo is a black texture made once. With DLSS upscaling (a mode below DLAA) the guides are at the GI's
// render size, each from the frame's pixel the GI's rays traced from (trace_common.hlsli gi_source), the motion in
// render pixels.

Texture2D<float> depth : register(t0);
RWTexture2D<float> rr_depth : register(u0);
RWTexture2D<float2> rr_motion : register(u1);
RWTexture2D<float4> rr_normal : register(u2);
RWTexture2D<float4> rr_albedo : register(u3);

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
    float4 reproject[4];  // render-offset space now -> the previous frame's clip space (row-vector)
    float4 prev;
    float4 gi_lighting[14];  // the GI constants this pass does not read (rtcore.cpp GiConstants: ambient .. terrain)
    float4 upscale;          // DLSS: xy the GI's render size (0: the frame's), zw the sample's offset (trace_common.hlsli)
};

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

// Where render-offset-space point p was in the previous frame, in pixels (false behind its camera).
bool previous_pixel(float3 p, out float2 pix) {
    const float4 c = p.x * reproject[0] + p.y * reproject[1] + p.z * reproject[2] + reproject[3];
    pix = 0.0;
    if (c.w <= 1e-4) return false;
    const float2 ndc = c.xy / c.w;
    pix = float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // the guide's pixel: the frame's, or with DLSS upscaling one of the render size's (as gi_source)
    const bool up = upscale.x > 0.0;
    if (any(id.xy >= (up ? uint2(upscale.xy) : size))) return;
    const float2 scale = up ? float2(size) / upscale.xy : float2(1.0, 1.0);  // frame pixels per guide pixel
    const int2 px = up ? clamp(int2(floor((float2(id.xy) + 0.5 + upscale.zw) * scale)), int2(0, 0), int2(size) - 1) : int2(id.xy);
    const float2 here = float2(px) + 0.5;
    const float z0 = view_depth(px);
    const float3 ray = view_ray(px);
    float2 then;
    if (z0 < 0.0) {
        const float3 dir = normalize(ray);
        rr_depth[id.xy] = 1e6;
        rr_albedo[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        rr_normal[id.xy] = float4(-dir, 1.0);
        rr_motion[id.xy] = previous_pixel(cam_origin.xyz + dir * 1e5, then) ? (then - here) / scale : float2(0.0, 0.0);
        return;
    }
    const float3 p0 = ray * z0;  // camera-relative
    // the surface's normal from the neighbouring depths, the side with the smaller step (as surface_at)
    const float zl = view_depth(px + int2(-1, 0)), zr = view_depth(px + int2(1, 0));
    const float zu = view_depth(px + int2(0, -1)), zd = view_depth(px + int2(0, 1));
    const float3 l = view_ray(px + int2(-1, 0)) * zl, r = view_ray(px + int2(1, 0)) * zr;
    const float3 u = view_ray(px + int2(0, -1)) * zu, d = view_ray(px + int2(0, 1)) * zd;
    const float3 dx = (zr > 0.0 && (zl < 0.0 || abs(zr - z0) < abs(z0 - zl))) ? r - p0 : (zl > 0.0 ? p0 - l : float3(0, 0, 0));
    const float3 dy = (zd > 0.0 && (zu < 0.0 || abs(zd - z0) < abs(z0 - zu))) ? d - p0 : (zu > 0.0 ? p0 - u : float3(0, 0, 0));
    float3 n = cross(dy, dx);
    n = dot(n, n) > 1e-12 ? normalize(n) : -normalize(ray);
    if (dot(n, -p0) < 0.0) n = -n;
    rr_depth[id.xy] = z0;
    rr_albedo[id.xy] = float4(1.0, 1.0, 1.0, 1.0);
    rr_normal[id.xy] = float4(n, 1.0);
    rr_motion[id.xy] = previous_pixel(cam_origin.xyz + p0, then) ? (then - here) / scale : float2(0.0, 0.0);
}
