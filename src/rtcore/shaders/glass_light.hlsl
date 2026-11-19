// The light of the glass (research/docs/04 section 6.1), pass 1 of 2. The game draws its plain glass (rfg-glass:
// windows, their shards) after the lit passes with a lighting of its own: its flat ambient colours, no sun but a fake
// bounce of it, and the sun's highlight with no shadow at all. Next to the path-traced scene that glass glowed in the
// dark and took no shadow. Here a ray from the camera finds the nearest clear glass in front of each pixel's opaque
// surface: the glass-only instances and the clear glass geometries of the others, all non-opaque in the rays' scene
// (tlas.cpp; the translucent glass left out: it is drawn opaque, the surface itself). The light at that point is traced as the GI's is: the sun through shadow rays spread over its disk
// (from the side facing it: the glass is thin, its dirt lit from either side), the sky and the surroundings through
// cosine rays about the side facing the camera. Pass 2 (glass_filter.hlsl) smooths it into the texture the patched glass
// shader (patch/glass.h) reads.
//   traced (u0, the core's own RGBA32F).rgb: the light from the sky and the surroundings (not the sun's), averaged over
//     frames (10 % of each frame's) with the previous frame's at the same point of the glass (reprojected: prev_raw)
//   .a: glass_pack's bits (glass_common.hlsli): the view depth, how much of the sun reaches the glass (averaged over
//     frames as well, 25 % of each frame's), the cosine of its angle; 0: no glass in front of the pixel
// And what the glass reflects (Reflections.Glass): one ray in the mirror direction off its surface, the radiance there as
// a mirror shows it (mirror_radiance; the drawn sky where it gets out), weighted by Schlick's Fresnel term
// (Reflections.GlassReflectance at normal incidence) and the game's fog between the camera and the glass:
//   glass_refl (u7, the plugin's RGBA16F, the patched glass shader's t19): rgb that weighted radiance (the game's lit
//     colour units), a the Fresnel term; 0 where there is no glass or no reflection
// The frame's counters (u6) from uint 60: the pixels with glass in front, those of them mostly in the sun (for the log).

#include "trace_common.hlsli"
#include "glass_common.hlsli"
RWStructuredBuffer<uint> frame_counts : register(u6);
RWTexture2D<float4> glass_refl : register(u7);
Texture2D<float4> prev_raw : register(t3);  // `traced` as the previous frame left it (rtcore.cpp copies it before this pass)

// The previous frame's light of the glass at hp: its sample where hp was on screen then (`reproject`), if that frame saw
// glass there at about the view depth hp had for it (what the pack keeps). None after a cut (flags & 8: no history).
bool glass_history(float3 hp, out float4 old, out GlassSample prev) {
    old = float4(0.0, 0.0, 0.0, 0.0);
    prev.t = 0.0;
    prev.vis = 0.0;
    prev.cosine = 0.0;
    if ((flags & 8u) != 0u) return false;
    const float4 c = hp.x * reproject[0] + hp.y * reproject[1] + hp.z * reproject[2] + reproject[3];
    if (c.w <= 1e-3) return false;
    const float2 ndc = c.xy / c.w;
    const float2 pix = float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
    if (any(pix < 0.0) || any(pix >= float2(size))) return false;
    old = prev_raw[uint2(pix)];
    return all(old.rgb == old.rgb) && glass_unpack(old.a, prev) && abs(prev.t - c.w) < max(0.05 * c.w, 0.1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    const float3 dir = normalize(view_ray(px));
    float3 p;
    float tmax = 2000.0;
    if (position_at(px, p)) tmax = max(length(p - cam_origin.xyz) - 0.02, 0.0);
    RayDesc ray;
    ray.Origin = cam_origin.xyz;
    ray.Direction = dir;
    ray.TMin = 0.05;
    ray.TMax = tmax;
    // the clear glass geometries of the non-opaque instances, every geometry of the glass-only ones (tlas.cpp; only theirs
    // come up with RAY_FLAG_CULL_OPAQUE)
    RayQuery<RAY_FLAG_CULL_OPAQUE> m;
    m.TraceRayInline(scene, RAY_FLAG_CULL_OPAQUE, 0x01, ray);
    while (m.Proceed()) {
        const uint c = m.CandidateInstanceContributionToHitGroupIndex();
        const uint g = m.CandidateGeometryIndex();
        if (g < 23u && ((c >> g) & 1u) != 0u && !RTQ_TRANSLUCENT(m, c, g)) m.CommitNonOpaqueTriangleHit();
    }
    bool found = false;
    float t = 0.0;
    uint hitInstance = 0, hitGeometry = 0, hitPrimitive = 0;
    float3x4 hitToWorld = (float3x4)0;
    if (m.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        found = true;
        t = m.CommittedRayT();
        hitInstance = m.CommittedInstanceID();
        hitGeometry = m.CommittedGeometryIndex();
        hitPrimitive = m.CommittedPrimitiveIndex();
        hitToWorld = m.CommittedObjectToWorld3x4();
    }
    bool sunny = false;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    float4 reflected = float4(0.0, 0.0, 0.0, 0.0);
    if (found) {
        const float3 hp = cam_origin.xyz + dir * t;
        const float3 n = hit_normal(hitInstance, hitGeometry, hitPrimitive, hitToWorld, dir);  // facing the camera
        const float offset = 0.02 + 0.002 * t;
        float vis = 0.0, cosine = 0.0;
        if (sun.w != 0.0) {
            const float3 l = normalize(sun.xyz);
            const float c = dot(n, l);
            const float3 ns = c >= 0.0 ? n : -n;
            // two rays a frame over the sun's disk (sky_probe.w: the tangent of its angular radius, Shadows.SunAngle)
            const float3 ta = normalize(abs(l.y) < 0.99 ? cross(l, float3(0, 1, 0)) : cross(l, float3(1, 0, 0)));
            const float3 tb = cross(l, ta);
            float lit = 0.0;
            for (uint s = 0; s < 2u; ++s) {
                const uint g1 = hash((id.x * 19349663u) ^ (id.y * 83492791u) ^ ((shadow.w * 8u + s + 3u) * 73856093u));
                const uint g2 = hash(g1 + 0x7F4A7C15u);
                const float rr = sqrt(unit(g1)) * sky_probe.w, ph = 6.2831853 * unit(g2);
                const float3 d = normalize(l + ta * (rr * cos(ph)) + tb * (rr * sin(ph)));
                lit += 0.5 * light_through(hp + ns * offset, d);
            }
            vis = lit;
            cosine = abs(c);
        }
        const float3 o = hp + n * offset;
        const uint rays = max(uint(gi.x), 1u);
        float3 sum = 0.0;
        for (uint i = 0; i < rays; ++i) {
            const uint h1 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ ((shadow.w * 16u + i + 11u) * 83492791u));
            const uint h2 = hash(h1 + 0x9E3779B9u);
            const float3 d = cosine_dir(n, unit(h1), unit(h2));
            RayDesc r;
            r.Origin = o;
            r.Direction = d;
            r.TMin = 0.0;
            r.TMax = gi.y;
            RayQuery<RAY_FLAG_NONE> g;
            g.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, r);
            Panes panes = panes_none();
            RT_PROCEED_PANES(g, panes, kDiffuseSpread);
            const bool hit = g.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            const float3 through = panes_through(panes, hit ? g.CommittedRayT() : gi.y);  // (GI.WindowTint)
            if (!hit)
                sum += through * gi2.y * sky_radiance(d);
            else
                sum += through * gi2.x * shade_hit(o, d, g.CommittedRayT(), g.CommittedInstanceID(), g.CommittedGeometryIndex(),
                                                   g.CommittedPrimitiveIndex(), g.CommittedTriangleBarycentrics(),
                                                   g.CommittedObjectToWorld3x4(), h2 ^ 0x5BD1E995u);
        }
        float3 e = PI * sum / float(rays);  // cosine-weighted: E = pi * the mean radiance
        // over frames: this point of the glass as the previous frame saw it (glass_history: reprojected), so that the
        // averages follow the glass as the view moves (read in place until 0.1.17, they trailed behind it)
        const float z = dot(hp - cam_origin.xyz, cam_forward.xyz);  // its view depth, which the pack keeps
        float4 old;
        GlassSample prev;
        if (glass_history(hp, old, prev)) {
            e = lerp(old.rgb, e, 0.1);
            vis = lerp(prev.vis, vis, 0.25);
        }
        sunny = vis > 0.5;
        result = float4(e, glass_pack(z, vis, cosine));

        // What it reflects: the mirror direction off its surface (no lobe: a window is smooth), through clear glass and
        // partly through translucent glass as other rays go.
        if (glass.y > 0.0) {
            const float3 rd = reflect(dir, n);
            RayDesc r;
            r.Origin = o;
            r.Direction = rd;
            r.TMin = 0.0;
            r.TMax = 2000.0;
            RayQuery<RAY_FLAG_NONE> g;
            g.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, r);
            RT_PROCEED(g);
            float3 lr;
            // the reflection's footprint: a pixel's, from the camera to the glass and on (a mirror keeps the cone)
            const float pixelAngle = 2.0 / (proj.y * float(size.y));
            if (g.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                lr = sky_radiance(rd);
            else
                lr = mirror_radiance(o, rd, g.CommittedRayT(), g.CommittedInstanceID(), g.CommittedGeometryIndex(),
                                     g.CommittedPrimitiveIndex(), g.CommittedTriangleBarycentrics(), g.CommittedObjectToWorld3x4(),
                                     float2(pixelAngle * t, pixelAngle));
            const float f0 = saturate(glass.x);
            const float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(-dir, n)), 5.0);
            // as the game's colours: the radiance times pi (its lit colour, the drawn sky's), the fog's share left
            const float3 shown = PI * max(lr, 0.0) * fog_transmittance(hp - cam_origin.xyz);
            if (all(shown == shown)) reflected = float4(fresnel * shown, fresnel);
        }
    }
    traced[px] = result;
    glass_refl[px] = reflected;
    const uint seen = WaveActiveCountBits(found), sunlit = WaveActiveCountBits(sunny);
    if (WaveIsFirstLane() && seen) {
        InterlockedAdd(frame_counts[60], seen);
        InterlockedAdd(frame_counts[61], sunlit);
    }
}
