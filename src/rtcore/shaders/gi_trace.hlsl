// Global illumination (research/docs/04 section 6.1), pass 1 of 3: irradiance by path tracing. From each pixel's
// surface (position and normal from the game's depth) a few cosine-weighted rays a frame: one that escapes sees the
// sky, one that hits a surface sees the light that surface sends back (trace_common.hlsli: what the game drew there,
// else a diffuse surface under the sky and the sun). The irradiance goes to `traced` in the game's units: an open
// surface facing up gets the game's own ambient colour, so that the patched shaders, which use it as their ambient,
// keep the game's balance. Passes 2 and 3 average it over frames and neighbours (or DLSS Ray Reconstruction, which with
// a mode below DLAA gets the rays at its render size, each pixel tracing from the frame's pixel under a jittered point
// of it: gi_source, and upscales them).

#include "trace_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gi_size())) return;
    float3 p, n;
    if (!surface_at(gi_source(id.xy), p, n)) {
        traced[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float dist = length(p - cam_origin.xyz);
    const float3 origin = p + n * (0.02 + 0.002 * dist);
    const uint rays = max(uint(gi.x), 1u);
    float3 sum = 0.0;
    for (uint i = 0; i < rays; ++i) {
        const uint h1 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ ((shadow.w * 16u + i + 7u) * 83492791u));
        const uint h2 = hash(h1 + 0x9E3779B9u);
        const float3 dir = cosine_dir(n, unit(h1), unit(h2));
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = 0.0;
        ray.TMax = gi.y;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
        Panes panes = panes_none();
        RT_PROCEED_PANES(q, panes, kDiffuseSpread);
        const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        // the clear glass it crossed tints what it brings back (GI.WindowTint)
        const float3 through = panes_through(panes, hit ? q.CommittedRayT() : gi.y);
        if (!hit)
            sum += through * gi2.y * sky_radiance(dir);  // GI.Sky: the sky as a light
        else
            sum += through * gi2.x * shade_hit(origin, dir, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                                               q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(),
                                               q.CommittedObjectToWorld3x4(), h2 ^ 0x68E31DA4u);
    }
    traced[id.xy] = float4(PI * sum / float(rays), 1.0);  // cosine-weighted: E = pi * the mean radiance
}
