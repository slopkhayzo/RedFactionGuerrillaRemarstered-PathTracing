// The sun's light through clear glass (GI.WindowTint): the colour of the clear panes between each pixel's surface and the
// sun, at the texel the ray crosses each one (the user: "the light going through them colored with the color of the window
// itself ... at the texel it goes through, so it preserves the pattern of the glass texture"), for the patched lit
// shaders' t20 (patch/sun_tint.h: their sun's colour times it).
//   sun_tint (u7, the plugin's RGBA8): rgb what of the sun's light the glass lets through, 1 without glass; a 1 (traced)
// A pane passes what the game's own blend of it shows through: lerp(1, its colour, its alpha) (its texture times its
// material's colour; its texture's alpha), so clear glass passes nearly all, tinted glass its tint, dirt and paint less.
// Clear glass for the rays: the clear glass geometries of the non-opaque instances, every geometry of the glass-only ones
// (tlas.cpp; RAY_FLAG_CULL_OPAQUE: only those come up; the frosted
// ones left out, no direct light passes them at all). A pane's two faces count once (candidates within kPaneThickness of
// one counted). The sun's centre only: the shadow's softness is ClbTexture's. Opaque things along the ray are not looked
// at: where they stop the sun, the pixel is in shadow and its tint unused.
#include "trace_common.hlsli"
RWTexture2D<unorm float4> sun_tint : register(u7);
// (pane_pass, new_pane: trace_common.hlsli)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    float3 tint = float3(1.0, 1.0, 1.0);
    float3 p, n;
    if (sun.w != 0.0 && surface_at(px, p, n)) {
        const float3 l = normalize(sun.xyz);
        if (dot(n, l) > 0.0) {
            const float dist = length(p - cam_origin.xyz);
            const float width = 2.0 / (proj.y * float(size.y)) * dist;  // the pixel's footprint: the pattern as fine as it shows
            RayDesc ray;
            ray.Origin = p + n * (0.02 + 0.002 * dist);
            ray.Direction = l;
            ray.TMin = 0.0;
            ray.TMax = 2000.0;
            float4 seen = float4(-1e30, -1e30, -1e30, -1e30);
            uint count = 0u;
            RayQuery<RAY_FLAG_CULL_OPAQUE> b;  // the clear glass (the non-opaque instances' glass geometries)
            b.TraceRayInline(scene, RAY_FLAG_CULL_OPAQUE, 0x01, ray);
            while (b.Proceed()) {
                const uint c = b.CandidateInstanceContributionToHitGroupIndex();
                const uint g = b.CandidateGeometryIndex();
                if (g < 23u && ((c >> g) & 1u) != 0u && !RTQ_TRANSLUCENT(b, c, g) && count < 8u &&
                    new_pane(b.CandidateTriangleRayT(), seen, count))
                    tint *= pane_pass(b.CandidateInstanceID(), g, b.CandidatePrimitiveIndex(), b.CandidateTriangleBarycentrics(),
                                      b.CandidateObjectToWorld3x4(), l, width);
            }
        }
    }
    sun_tint[px] = float4(saturate(tint), 1.0);
}
