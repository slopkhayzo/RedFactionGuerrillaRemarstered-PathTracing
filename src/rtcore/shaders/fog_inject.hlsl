// The volumetric fog, pass 1 of 3: how much of the sun and of the sky reaches each froxel of a camera-aligned grid
// (fog3.x by fog3.y cells across the screen, fog2.w slices in depth from fog2.y to fog2.z metres, exponential). From a
// random point of the froxel each frame: a shadow ray toward a point of the sun's disk (sky_probe.w: the tangent of its
// angular radius, as the shadows'), and a ray up into a cosine-weighted direction (kSkyRay long: under a roof, indoors,
// it meets something). Averaged over frames with what the froxel's centre had in the previous frame (reprojected, read
// between froxels; fog3.z: that frame's grid is there; fog3.w frames at most).
//   u0 (RGBA16F): the sun's visibility, the sky's, the frames averaged
#include "fog_common.hlsli"
Texture3D<float4> fog_prev : register(t3);
RWTexture3D<float4> fog_out : register(u0);

static const float kSkyRay = 250.0;

bool open_toward(float3 o, float3 d, float tmax) {
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint3 n = froxel_count();
    if (any(id >= n)) return;
    const uint h0 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ (id.z * 83492791u) ^ (shadow.w * 2654435761u));
    const uint h1 = hash(h0 + 0x9E3779B9u), h2 = hash(h1 + 0x9E3779B9u), h3 = hash(h2 + 0x9E3779B9u);
    const uint h4 = hash(h3 + 0x9E3779B9u), h5 = hash(h4 + 0x9E3779B9u), h6 = hash(h5 + 0x9E3779B9u);
    const float2 uv = (float2(id.xy) + float2(unit(h0), unit(h1))) / float2(n.xy);
    const float3 p = cam_origin.xyz + view_ray_uv(uv) * slice_depth(float(id.z) + unit(h2));
    float vs = 0.0;
    if (sun.w != 0.0) {
        const float3 l = normalize(sun.xyz);
        const float3 ta = normalize(abs(l.y) < 0.99 ? cross(l, float3(0, 1, 0)) : cross(l, float3(1, 0, 0)));
        const float3 tb = cross(l, ta);
        const float rr = sqrt(unit(h3)) * sky_probe.w, ph = 6.2831853 * unit(h4);
        vs = light_through(p, normalize(l + ta * (rr * cos(ph)) + tb * (rr * sin(ph))));  // (smooth through glass)
    }
    const float vk = open_toward(p, cosine_dir(float3(0, 1, 0), unit(h5), unit(h6)), kSkyRay) ? 1.0 : 0.0;
    float4 hist = 0.0;
    if (fog3.z != 0.0) {  // the froxel's centre in the previous frame's grid
        const float3 pc = cam_origin.xyz + view_ray_uv((float2(id.xy) + 0.5) / float2(n.xy)) * slice_depth(float(id.z) + 0.5);
        const float4 c = pc.x * reproject[0] + pc.y * reproject[1] + pc.z * reproject[2] + reproject[3];
        if (c.w > 1e-3) {
            const float2 ndc = c.xy / c.w;
            const float3 tc = float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, depth_slice(c.w) / float(n.z));
            if (all(tc >= 0.0) && all(tc <= 1.0)) hist = fog_prev.SampleLevel(linear_clamp, tc, 0);
        }
    }
    const float frames = min(hist.z + 1.0, max(fog3.w, 1.0));
    const float a = 1.0 / frames;
    fog_out[id] = float4(lerp(hist.x, vs, a), lerp(hist.y, vk, a), frames, 0.0);
}
