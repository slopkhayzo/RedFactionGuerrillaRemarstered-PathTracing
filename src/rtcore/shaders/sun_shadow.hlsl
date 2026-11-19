// Phases 3 and 4 (research/docs/03): ray-traced sun shadows and ambient occlusion, pass 1 of 3. Each pixel's position
// comes back from the game's depth, its normal from the neighbouring depths. A few rays toward the sun, spread over its
// disk, say how much of it the pixel sees; a few rays over the hemisphere around the normal, out to a short radius, say
// how much of the ambient light nothing close by hides. Both go to `visibility` (x sun, y ambient); pass 2
// (sun_shadow_temporal.hlsl) averages them over frames, pass 3 (sun_shadow_filter.hlsl) over neighbours, into
// ClbTexture's sun-shadow (G) and ambient-occlusion (R) channels. rl_light::m_light_direction points toward the sun
// (2026-11-13: 87.5% agreement with the game's cascades that way, 35% the other).

#define RT_INSTANCE_TABLE instances  // (rt_query.hlsli: which glass geometries are translucent)
#include "rt_query.hlsli"
RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> depth : register(t1);
StructuredBuffer<uint4> instances : register(t3);  // the TLAS's instance table (trace_common.hlsli): x bits 8-31
RWTexture2D<unorm float2> visibility : register(u0);

cbuffer Frame : register(b0) {
    float4 cam_right;  // xyz: camera axes and position (render-offset space)
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;       // P00, P11, P22, P32 of the game's projection
    float4 range;      // x: the main scene's depth range end; y: the AO rays' length (m); z: AO rays a pixel (0: none)
    float4 sun;        // xyz: toward the sun (rl_light::m_light_direction); w: 1 when there is a sun
    uint2 size;
    float tolerance;
    uint flags;        // 2: reversed depth
    uint4 shadow;      // x: rays per pixel, y: tan of the sun's angular radius (as float bits), z: filter radius, w: frame
};

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;  // view depth 1
}

// The surface at pixel p, in render-offset space; false for sky and the landmark layer.
bool position_at(int2 p, out float3 pos) {
    pos = 0;
    if (any(p < 0) || any(p >= int2(size))) return false;
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return false;
    const float z = proj.w / (d - proj.z);
    pos = cam_origin.xyz + view_ray(p) * z;
    return true;
}

// How much of the sun gets through along dir: 0 behind anything opaque, the translucent glass's share for each pane
// crossed (RT_THROUGH: smooth, not the random share of the rays that pass).
float sun_through(float3 origin, float3 dir) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = 5000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    float through = 1.0;
    RT_THROUGH(q, through);
    return through;
}

// The nearest surface along dir within tmax; its distance in t.
bool hit_within(float3 origin, float3 dir, float tmax, out float t) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    t = q.CommittedRayT();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Frosted glass (RTQ_FROSTED): whether the pixel's surface, `dist` along the camera's ray through it, is a translucent pane
// (the camera's ray stops on one there, as the passes' rays do).
bool on_pane(float3 p, float dist) {
    RayDesc ray;
    ray.Origin = cam_origin.xyz;
    ray.Direction = (p - cam_origin.xyz) / max(dist, 1e-4);
    ray.TMin = 0.0;
    ray.TMax = dist + 0.5;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    const uint c = q.CommittedInstanceContributionToHitGroupIndex();
    const uint g = q.CommittedGeometryIndex();
    if (((c >> 23) & 1u) == 0u || g >= 24u) return false;
    if ((((instances[q.CommittedInstanceID()].x >> 8) >> g) & 1u) == 0u) return false;
    return abs(q.CommittedRayT() - dist) < max(0.02 * dist, 0.1);
}

// A frosted pane in shade still glows with the sky its far side sees (the game gives its glow a floor of 0.3 of the sun
// for that): this share of it.
static const float kPaneSky = 0.3;

uint hash(uint x) {  // lowbias32
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    float3 p;
    if (!position_at(px, p)) {
        visibility[id.xy] = float2(1.0, 1.0);
        return;
    }
    // The normal: from the neighbours on the side with the smaller step, so that edges do not bend it.
    float3 l, r, u, d;
    const bool hl = position_at(px + int2(-1, 0), l), hr = position_at(px + int2(1, 0), r);
    const bool hu = position_at(px + int2(0, -1), u), hd = position_at(px + int2(0, 1), d);
    const float3 dx = (hr && (!hl || length(r - p) < length(p - l))) ? r - p : (hl ? p - l : float3(1, 0, 0));
    const float3 dy = (hd && (!hu || length(d - p) < length(p - u))) ? d - p : (hu ? p - u : float3(0, 0, 1));
    float3 n = normalize(cross(dy, dx));
    if (dot(n, cam_origin.xyz - p) < 0.0) n = -n;
    const float dist = length(p - cam_origin.xyz);
    const float3 origin = p + n * (0.02 + 0.002 * dist);

    // The sun: directions over its disk, points of a rotated golden-angle spiral (rotation per pixel and frame, so that
    // the filter and successive frames see different ones).
    float sun_seen = 1.0;
    if (sun.w != 0.0) {
        const float3 to_sun = normalize(sun.xyz);
        const float3 t = normalize(abs(to_sun.y) < 0.99 ? cross(to_sun, float3(0, 1, 0)) : cross(to_sun, float3(1, 0, 0)));
        const float3 b = cross(to_sun, t);
        const uint rays = max(shadow.x, 1u);
        const float spread = asfloat(shadow.y);
        const float rotation = float(hash(id.x + id.y * 4099u + shadow.w * 16777619u) & 0xFFFFu) / 65536.0 * 6.2831853;
        float lit = 0.0;
        for (uint i = 0; i < rays; ++i) {
            const float radius = sqrt((float(i) + 0.5) / float(rays));
            const float angle = rotation + float(i) * 2.3999632;  // golden angle
            const float3 dir = normalize(to_sun + (t * cos(angle) + b * sin(angle)) * (radius * spread));
            lit += sun_through(origin, dir);
        }
        sun_seen = lit / float(rays);
    }
    // a frosted pane: plus the sky beyond it (one cosine ray a frame about its far side's normal, from beyond it: the
    // frames' average and the filter smooth it), so that a pane out of the sun glows as the patterned glass under the sky
    // does, and one between closed rooms does not
    if (RTQ_FROSTED && sun_seen < 1.0 && on_pane(p, dist)) {
        const float3 nf = -n;
        const float3 tn = normalize(abs(nf.y) < 0.99 ? cross(nf, float3(0, 1, 0)) : cross(nf, float3(1, 0, 0)));
        const float3 bn = cross(nf, tn);
        const uint h1 = hash((id.x * 19349663u) ^ (id.y * 73856093u) ^ ((shadow.w * 4u + 3u) * 83492791u));
        const uint h2 = hash(h1 + 0x9E3779B9u);
        const float u1 = float(h1 & 0xFFFFFFu) / 16777216.0, u2 = float(h2 & 0xFFFFFFu) / 16777216.0;
        const float rr = sqrt(u1), phi = 6.2831853 * u2;
        const float3 dir = normalize(tn * (rr * cos(phi)) + bn * (rr * sin(phi)) + nf * sqrt(max(1.0 - u1, 0.0)));
        float th;
        const bool sky = !hit_within(p + nf * (kPaneSelf + 0.05), dir, 500.0, th);
        sun_seen = saturate(sun_seen + kPaneSky * (sky ? 1.0 : 0.0));
    }

    // Ambient occlusion: cosine-weighted directions over the hemisphere around the normal, random per pixel and frame;
    // a surface within the radius occludes by how close it is (1 - (t / radius)^2).
    float ambient = 1.0;
    const uint ao_rays = uint(range.z);
    if (ao_rays > 0) {
        const float radius = max(range.y, 0.05);
        const float3 tn = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
        const float3 bn = cross(n, tn);
        float occluded = 0.0;
        for (uint i = 0; i < ao_rays; ++i) {
            const uint h1 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ ((shadow.w * 8u + i) * 83492791u));
            const uint h2 = hash(h1 + 0x9E3779B9u);
            const float u1 = float(h1 & 0xFFFFFFu) / 16777216.0, u2 = float(h2 & 0xFFFFFFu) / 16777216.0;
            const float rr = sqrt(u1), phi = 6.2831853 * u2;
            const float3 dir = normalize(tn * (rr * cos(phi)) + bn * (rr * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
            float th;
            if (hit_within(origin, dir, radius, th)) {
                const float f = th / radius;
                occluded += 1.0 - f * f;
            }
        }
        ambient = 1.0 - occluded / float(ao_rays);
    }
    visibility[id.xy] = float2(sun_seen, ambient);
}
