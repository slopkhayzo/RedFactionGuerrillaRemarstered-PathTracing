// Test.ProbeKey: what the rays meet under the crosshair (the screen's centre; Test.ProbePixel: another point), for the log. From the surface the game
// drew there toward the sun: what shadows it. From the camera through it: what the rays take for the first surface
// (nearer than the game's: something in the rays' scene the game does not show; farther, or nothing: what the game
// drew there is not in the rays' scene). Into the frame's counters from uint 32 (u6), read back with them.

#include "trace_common.hlsli"

RWStructuredBuffer<uint> probe_out : register(u6);

[numthreads(1, 1, 1)]
void main() {
    const int2 c = int2(clamp(probe.xy, 0.0, 0.999) * float2(size));
    float3 p, n;
    const bool surf = surface_at(c, p, n);
    const float dist = surf ? length(p - cam_origin.xyz) : 0.0;
    uint sunInst = 0xFFFFFFFFu, sunGeom = 0u, camInst = 0xFFFFFFFFu, camGeom = 0u;
    float sunT = 0.0, camT = 0.0;
    probe_out[75] = 0u;  // (an earlier probe's surface is not this one's)
    if (surf && sun.w != 0.0) {
        RayDesc ray;
        ray.Origin = p + n * (0.02 + 0.002 * dist);
        ray.Direction = normalize(sun.xyz);
        ray.TMin = 0.0;
        ray.TMax = 5000.0;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
        RT_PROCEED(q);
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            sunInst = q.CommittedInstanceID();
            sunGeom = q.CommittedGeometryIndex();
            sunT = q.CommittedRayT();
        }
    }
    {
        RayDesc ray;
        ray.Origin = cam_origin.xyz;
        ray.Direction = normalize(view_ray(c));
        ray.TMin = 0.0;
        ray.TMax = 5000.0;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
        RT_PROCEED(q);
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            camInst = q.CommittedInstanceID();
            camGeom = q.CommittedGeometryIndex();
            camT = q.CommittedRayT();
            // what the rays make of that surface (hit_surface): its albedo and self-illumination, from uint 64, then its
            // geometry table's header and material entry, and the entry itself; uint 75 says they are there
            const Surface s = hit_surface(camInst, camGeom, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(),
                                          q.CommittedObjectToWorld3x4(), ray.Direction, 2.0 / (proj.y * float(size.y)) * camT);
            probe_out[64] = asuint(s.albedo.r);
            probe_out[65] = asuint(s.albedo.g);
            probe_out[66] = asuint(s.albedo.b);
            probe_out[67] = asuint(s.glow);
            const uint4 gi4 = instances[camInst];
            const uint page = gi4.x & 0xFFu;
            uint4 mat = uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
            uint2 head = uint2(0xFFFFFFFFu, 0xFFFFFFFFu), geo = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
            if (page < 64u) {
                head = pages[NonUniformResourceIndex(page)].Load2(gi4.w);
                geo = pages[NonUniformResourceIndex(page)].Load2(gi4.w + 8 + camGeom * 8);
                if (head.y < kMaterialSlots && geo.y < kMaterialEntries) mat = materials[head.y * kMaterialEntries + geo.y];
            }
            probe_out[68] = head.x;
            probe_out[69] = head.y;
            probe_out[70] = geo.y;
            probe_out[71] = mat.x;
            probe_out[72] = mat.y;
            probe_out[73] = mat.z;
            probe_out[74] = mat.w;
            // the calibration: the game's colour there (the previous frame's, pi times the radiance) beside what the rays
            // make of it (its material under the sun and the ambient, as a mirror shows it), from uint 81
            float3 seen = float3(-1.0, -1.0, -1.0);
            float3 lseen;
            if (seen_radiance(ray.Origin + ray.Direction * camT, lseen)) seen = PI * lseen;
            const float3 hp = ray.Origin + ray.Direction * camT + s.n * (0.02 + 0.002 * camT);
            const float3 sky = gi2.y * lerp(back_ambient.rgb, ambient.rgb, saturate(s.n.y * 0.5 + 0.5));
            const float3 made = PI * surface_radiance(s, sun_at(hp, s.n) + sky);
            probe_out[81] = asuint(seen.r);
            probe_out[82] = asuint(seen.g);
            probe_out[83] = asuint(seen.b);
            probe_out[84] = asuint(made.r);
            probe_out[85] = asuint(made.g);
            probe_out[86] = asuint(made.b);
            probe_out[75] = 0x4D41544Cu;  // "MATL"
        }
    }
    // and the first surface of any kind along it, glass included (every candidate committed, both masks)
    uint anyInst = 0xFFFFFFFFu, anyGeom = 0u;
    float anyT = 0.0;
    probe_out[80] = 0u;
    {
        RayDesc ray;
        ray.Origin = cam_origin.xyz;
        ray.Direction = normalize(view_ray(c));
        ray.TMin = 0.0;
        ray.TMax = 5000.0;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0x03, ray);
        while (q.Proceed()) q.CommitNonOpaqueTriangleHit();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            anyInst = q.CommittedInstanceID();
            anyGeom = q.CommittedGeometryIndex();
            anyT = q.CommittedRayT();
            // what a mirror there would show (glass_light.hlsl's reflection, when it is glass), from uint 76: the mirror
            // ray's hit (instance, geometry, distance) and how it is shaded: 1 the game's colour where it saw it, 2 its
            // material's texture, 3 its material without a texture, 4 not known (the grey); uint 80 says they are there
            const float3 n = hit_normal(anyInst, anyGeom, q.CommittedPrimitiveIndex(), q.CommittedObjectToWorld3x4(), ray.Direction);
            const float3 o = cam_origin.xyz + ray.Direction * anyT + n * (0.02 + 0.002 * anyT);
            RayDesc r;
            r.Origin = o;
            r.Direction = reflect(ray.Direction, n);
            r.TMin = 0.0;
            r.TMax = 2000.0;
            RayQuery<RAY_FLAG_NONE> m;
            m.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, r);
            RT_PROCEED(m);
            uint how = 0u, mInst = 0xFFFFFFFFu, mGeom = 0u;
            float mT = 0.0;
            // and, from uint 87, that surface three ways (pi times the radiance; -1: not there): the game's colour of it
            // (on screen in the previous frame), the radiance cache's, what its material makes under the sun and the
            // ambient (mirror_radiance's own); then its albedo and whether its material is known (uint 99)
            float3 mSeen = float3(-1.0, -1.0, -1.0), mCache = float3(-1.0, -1.0, -1.0), mMade = float3(-1.0, -1.0, -1.0);
            float3 mAlbedo = float3(-1.0, -1.0, -1.0);
            uint mKnown = 0u;
            if (m.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                mInst = m.CommittedInstanceID();
                mGeom = m.CommittedGeometryIndex();
                mT = m.CommittedRayT();
                const float3 mp = o + r.Direction * mT;
                const float pixelAngle = 2.0 / (proj.y * float(size.y));
                const Surface ms = hit_surface(mInst, mGeom, m.CommittedPrimitiveIndex(), m.CommittedTriangleBarycentrics(),
                                               m.CommittedObjectToWorld3x4(), r.Direction, pixelAngle * (anyT + mT));
                float3 lm;
                if (seen_radiance(mp, lm)) mSeen = PI * lm;
                const bool mCached = cache_radiance(mp, ms.n, lm);
                if (mCached) mCache = PI * lm;
                mMade = PI * mirror_shade(ms, mp + ms.n * (0.02 + 0.002 * mT), mCached, lm);  // as mirror_radiance makes it
                mAlbedo = ms.albedo;
                mKnown = ms.known ? 1u : 0u;
                float3 seen;
                if (seen_radiance(o + r.Direction * mT, seen)) {
                    how = 1u;
                } else {
                    how = 4u;
                    const uint4 g4 = instances[mInst];
                    const uint pg = g4.x & 0xFFu;
                    if (pg < 64u) {
                        const uint2 hd = pages[NonUniformResourceIndex(pg)].Load2(g4.w);
                        const uint2 ge = pages[NonUniformResourceIndex(pg)].Load2(g4.w + 8 + mGeom * 8);
                        if (hd.y < kMaterialSlots && ge.y < kMaterialEntries) {
                            const uint4 mat = materials[hd.y * kMaterialEntries + ge.y];
                            if ((mat.x & 0x80000000u) != 0u) how = (mat.x & 0xFFFFu) != 0u && hd.x != 0xFFFFFFFFu ? 2u : 3u;
                        }
                    }
                }
            }
            probe_out[76] = mInst;
            probe_out[77] = mGeom;
            probe_out[78] = asuint(mT);
            probe_out[79] = how;
            probe_out[87] = asuint(mSeen.r);
            probe_out[88] = asuint(mSeen.g);
            probe_out[89] = asuint(mSeen.b);
            probe_out[90] = asuint(mCache.r);
            probe_out[91] = asuint(mCache.g);
            probe_out[92] = asuint(mCache.b);
            probe_out[93] = asuint(mMade.r);
            probe_out[94] = asuint(mMade.g);
            probe_out[95] = asuint(mMade.b);
            probe_out[96] = asuint(mAlbedo.r);
            probe_out[97] = asuint(mAlbedo.g);
            probe_out[98] = asuint(mAlbedo.b);
            probe_out[99] = mKnown;
            probe_out[80] = 0x4D495252u;  // "MIRR"
        }
    }
    probe_out[45] = anyInst;
    probe_out[46] = anyGeom;
    probe_out[47] = asuint(anyT);
    probe_out[32] = 0x50524F42u;  // "PROB"
    probe_out[33] = surf ? 1u : 0u;
    probe_out[34] = asuint(dist);
    probe_out[35] = sunInst;
    probe_out[36] = sunGeom;
    probe_out[37] = asuint(sunT);
    probe_out[38] = camInst;
    probe_out[39] = camGeom;
    probe_out[40] = asuint(camT);
    probe_out[41] = sun.w != 0.0 ? 1u : 0u;
    probe_out[42] = asuint(p.x);
    probe_out[43] = asuint(p.y);
    probe_out[44] = asuint(p.z);
}
