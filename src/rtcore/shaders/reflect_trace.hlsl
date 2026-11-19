// Reflections (research/docs/04 section 6.1), pass 1 of 3: what each pixel's specular reflects. The game's lit shaders
// take their specular ambient as its hemispherical sky looked up along the reflection vector (lerp(backAmbient, ambient,
// R.up), the fake bounce blended in), times the specular map, the fresnel and indirectLightParams.z; patched
// (patch/ambient.h, patch_reflection), they read this instead at t16: what a ray along the reflection vector meets, in
// the same units, pi times its radiance (the sky the game's own value, a surface the game drew its lit colour).
//
// One ray a pixel and frame, about the mirror direction off the surface's geometric normal (from the depth: the normal
// map's detail stays the shader's own, in its fresnel and specular map), spread over a cosine-power lobe of exponent
// gi.x; passes 2 and 3 (gi_temporal, gi_filter) average it over a few frames and neighbours.

#include "trace_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float3 p, n;
    if (!surface_at(int2(id.xy), p, n)) {
        traced[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float3 v = normalize(p - cam_origin.xyz);
    const float3 r0 = reflect(v, n);
    const uint h1 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ ((shadow.w * 16u + 11u) * 83492791u));
    const uint h2 = hash(h1 + 0x9E3779B9u);
    const float u1 = float(h1 & 0xFFFFFFu) / 16777216.0, u2 = float(h2 & 0xFFFFFFu) / 16777216.0;
    const float e = max(gi.x, 1.0);
    const float cos_t = pow(max(u1, 1e-6), 1.0 / (e + 1.0)), sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));
    const float phi = 6.2831853 * u2;
    const float3 t1 = normalize(abs(r0.y) < 0.99 ? cross(r0, float3(0, 1, 0)) : cross(r0, float3(1, 0, 0)));
    const float3 b1 = cross(r0, t1);
    float3 dir = normalize(t1 * (sin_t * cos(phi)) + b1 * (sin_t * sin(phi)) + r0 * cos_t);
    if (dot(dir, n) <= 0.0) dir = r0;  // below the surface: the mirror direction itself
    const float dist = length(p - cam_origin.xyz);
    const float3 origin = p + n * (0.02 + 0.002 * dist);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = gi.y;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    Panes panes = panes_none();
    RT_PROCEED_PANES(q, panes, kDiffuseSpread);
    float3 env;
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float3 through = panes_through(panes, hit ? q.CommittedRayT() : gi.y);  // the clear glass it crossed (GI.WindowTint)
    if (!hit)
        env = PI * sky_drawn(dir, lerp(back_ambient.rgb, ambient.rgb, saturate(dir.y * 0.5 + 0.5)) / PI, ambient.rgb / PI);  // the sky
    else
        env = PI * shade_hit(origin, dir, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                             q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(),
                             h2 ^ 0x68E31DA4u);
    traced[id.xy] = float4(through * env, 1.0);
}
