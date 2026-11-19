#pragma once
// Rays go through glass and every other transparent material (rl_material_data::mat_flags bit 0, as
// rl_material::get_is_transparent reads it). The TLAS (tlas.cpp) makes an instance that draws some of them non-opaque
// (FORCE_NON_OPAQUE), with which of its BLAS geometries are glass in InstanceContributionToHitGroupIndex (bit g: geometry
// g, the first 23; inline rays use no hit groups); one that draws only such ranges has them all (more than 23 geometries:
// out of mask 0x01). Their candidates are committed unless glass; every other instance is opaque and never stops this loop.
// Translucent glass (rfg-eosglass, the patterned glass of the multiplayer maps; bit 23 of the contribution: the instance
// has some) stops a share of the light (kTranslucentOpacity) a pane, the clear glass beside it none. Which of an
// instance's glass geometries are translucent: bits 8-31 of its instance table entry's x (tlas.cpp), where the pass has
// the table (RT_INSTANCE_TABLE, its name); without it, all of an instance with bit 23 are.
static const float kTranslucentOpacity = 0.7;
// Translucent candidates nearer to each other than this along a ray are one pane: a pane's two faces, its triangles'
// shared edges, a triangle reported twice.
static const float kPaneThickness = 0.25;
// Frosted glass: translucent candidates nearer than this to a ray's origin are the pane the ray starts on (its own other
// face: the dome's panes are 30 cm and more across along a slanted ray).
static const float kPaneSelf = 0.6;

// Frosted glass (the pass's flags & 32, GI.FrostedGlass): a translucent pane diffuses the light through it. It stops
// every ray, the sun's included (no direct light through it); the light it lets through comes from the shading of a hit
// on it (trace_common.hlsli pane_radiance: its far side's light, through its texture). But not the pane a ray starts on
// (candidates nearer than kPaneSelf: its other face), so that a pane's own sun visibility stays its outer face's.
// (`flags`: the including pass's constants.)
#define RTQ_FROSTED ((flags & 32u) != 0u)

uint rtq_hash(uint x) {  // lowbias32
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

#ifdef RT_INSTANCE_TABLE
#define RTQ_TRANSLUCENT(q, c, g) \
    ((((c) >> 23) & 1u) != 0u && (((RT_INSTANCE_TABLE[q.CandidateInstanceID()].x >> 8) >> (g)) & 1u) != 0u)
#else
#define RTQ_TRANSLUCENT(q, c, g) ((((c) >> 23) & 1u) != 0u)
#endif

// A shadow ray's way (RT_THROUGH): the share of the light that gets through, each translucent pane crossed letting
// 1 - kTranslucentOpacity of it pass (counted once: its other candidates within kPaneThickness are skipped), clear glass
// all of it, 0 at anything opaque: the light through that glass comes smooth, frame after frame, rather than as the
// random share of the rays that pass; none through a frosted one (RTQ_FROSTED). `q` with
// RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH; `through` (float) starts at 1.
#define RT_THROUGH(q, through)                                                                                    \
    {                                                                                                             \
        float rtq_t0 = -1e30, rtq_t1 = -1e30, rtq_t2 = -1e30, rtq_t3 = -1e30;                                     \
        uint rtq_n = 0u;                                                                                          \
        while (q.Proceed()) {                                                                                     \
            const uint rtq_c = q.CandidateInstanceContributionToHitGroupIndex();                                  \
            const uint rtq_g = q.CandidateGeometryIndex();                                                        \
            if (rtq_g >= 23u || !((rtq_c >> rtq_g) & 1u)) {                                                       \
                q.CommitNonOpaqueTriangleHit();                                                                   \
            } else if (RTQ_TRANSLUCENT(q, rtq_c, rtq_g)) {                                                        \
                const float rtq_d = q.CandidateTriangleRayT();                                                    \
                if (RTQ_FROSTED) {                                                                                \
                    if (rtq_d >= kPaneSelf) q.CommitNonOpaqueTriangleHit();                                       \
                } else if (abs(rtq_d - rtq_t0) >= kPaneThickness && abs(rtq_d - rtq_t1) >= kPaneThickness &&     \
                           abs(rtq_d - rtq_t2) >= kPaneThickness && abs(rtq_d - rtq_t3) >= kPaneThickness) {      \
                    through *= 1.0 - kTranslucentOpacity;                                                         \
                    if (rtq_n == 0u) rtq_t0 = rtq_d;                                                              \
                    else if (rtq_n == 1u) rtq_t1 = rtq_d;                                                         \
                    else if (rtq_n == 2u) rtq_t2 = rtq_d;                                                         \
                    else rtq_t3 = rtq_d;                                                                          \
                    ++rtq_n;                                                                                      \
                }                                                                                                 \
            }                                                                                                     \
        }                                                                                                         \
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) through = 0.0;                                         \
    }

// A translucent pane's candidate on every other ray's way (RT_PROCEED; trace_common.hlsli RT_PROCEED_PANES): committed
// (the pane stops the ray) for kTranslucentOpacity of the rays, chosen by a hash of the ray and the pane (its distance
// along the ray in steps of kPaneThickness: its two faces decide alike), or for all of them when frosted (RTQ_FROSTED: a
// surface that glows with the light through it), beyond the pane the ray starts on.
#define RTQ_TRANSLUCENT_STEP(q)                                                                                   \
    if (RTQ_FROSTED) {                                                                                            \
        if (q.CandidateTriangleRayT() >= kPaneSelf) q.CommitNonOpaqueTriangleHit();                               \
    } else {                                                                                                      \
        const uint rtq_h = rtq_hash(asuint(q.WorldRayDirection().x) ^                                             \
                                    rtq_hash(asuint(q.WorldRayDirection().y) ^ asuint(q.WorldRayOrigin().z) ^     \
                                             uint(q.CandidateTriangleRayT() / kPaneThickness)));                  \
        if (float(rtq_h & 0xFFFFFFu) < kTranslucentOpacity * 16777216.0) q.CommitNonOpaqueTriangleHit();         \
    }

// Every other ray's way: a translucent pane stops a share of the rays (RTQ_TRANSLUCENT_STEP), clear glass none.
#define RT_PROCEED(q)                                                                                             \
    while (q.Proceed()) {                                                                                         \
        const uint rtq_c = q.CandidateInstanceContributionToHitGroupIndex();                                      \
        const uint rtq_g = q.CandidateGeometryIndex();                                                            \
        if (rtq_g >= 23u || !((rtq_c >> rtq_g) & 1u)) {                                                           \
            q.CommitNonOpaqueTriangleHit();                                                                       \
        } else if (RTQ_TRANSLUCENT(q, rtq_c, rtq_g)) {                                                            \
            RTQ_TRANSLUCENT_STEP(q)                                                                               \
        }                                                                                                         \
    }
