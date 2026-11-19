// Shadows for the dynamic lights (research/docs/04 section 6.1): for each light the frame scene lists (a point or spot
// rl_light the game may draw this frame, frame_scene.h), the visibility of the light from each pixel's surface, into
// that light's mask (G; R stays 1). The plugin binds the mask at t15 for the light's own pass, where the game's light
// shaders multiply by ClbTexture.G (the 93 that did not are patched to, patch/light_pass.h).
//
// One ray a pixel and light toward the light's centre: hard shadows, without noise to filter. A light's lamp usually
// surrounds it, so what lies within `fixture` metres of the light does not shadow it.

#include "rt_query.hlsli"
RaytracingAccelerationStructure scene : register(t5);
Texture2D<float> depth : register(t0);
RWTexture2D<unorm float2> masks[16] : register(u0);

struct Light {
    float3 position;   // render-offset space
    float radius;      // the attenuation's end
    float3 direction;  // spot: where it points; zero for omni
    float cos_cone;    // spot: the cone's outer cosine; -1 for omni
    uint mask;
    uint flags;
    float source_radius;
    float fixture;
};

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
    uint flags;           // 2: reversed depth
    uint4 shadow;         // w: frame
    float4 reproject[4];
    float4 prev;
    uint4 light_info;     // x: lights, y: masks to write (every mask in use is written, lit or not)
    Light lights[16];
};

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

bool position_at(int2 p, out float3 pos) {
    pos = 0;
    if (any(p < 0) || any(p >= int2(size))) return false;
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return false;
    pos = cam_origin.xyz + view_ray(p) * (proj.w / (d - proj.z));
    return true;
}

bool blocked(float3 o, float3 d, float t_max) {
    if (t_max <= 0.0) return false;
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = t_max;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    const uint count = min(light_info.x, 16u);
    float3 p;
    if (!position_at(px, p)) {
        for (uint i = 0; i < count; ++i) masks[lights[i].mask][id.xy] = float2(1.0, 1.0);
        return;
    }
    float3 l, r, u, d;
    const bool hl = position_at(px + int2(-1, 0), l), hr = position_at(px + int2(1, 0), r);
    const bool hu = position_at(px + int2(0, -1), u), hd = position_at(px + int2(0, 1), d);
    const float3 dx = (hr && (!hl || length(r - p) < length(p - l))) ? r - p : (hl ? p - l : float3(1, 0, 0));
    const float3 dy = (hd && (!hu || length(d - p) < length(p - u))) ? d - p : (hu ? p - u : float3(0, 0, 1));
    float3 n = normalize(cross(dy, dx));
    if (dot(n, cam_origin.xyz - p) < 0.0) n = -n;
    const float bias = 0.02 + 0.002 * length(p - cam_origin.xyz);
    for (uint i = 0; i < count; ++i) {
        const Light L = lights[i];
        float visible = 1.0;
        const float3 to = L.position - p;
        const float dist = length(to);
        if (dist < L.radius && dist > 1e-3) {
            const float3 dir = to / dist;
            // facing away (the game's N.L takes it to black), or outside a spot's cone: nothing to shadow
            const bool lit_side = dot(n, dir) > 0.0;
            const bool in_cone = L.cos_cone <= -1.0 || dot(-dir, L.direction) >= L.cos_cone;
            if (lit_side && in_cone && blocked(p + n * bias, dir, dist - max(L.fixture, bias))) visible = 0.0;
        }
        masks[L.mask][id.xy] = float2(1.0, visible);
    }
}
