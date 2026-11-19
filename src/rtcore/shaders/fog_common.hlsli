// The volumetric fog's shared parts (fog_inject.hlsl, fog_integrate.hlsl, fog_apply.hlsl): the game's fog as its shaders
// compute it, the camera-aligned grid of froxels, how the fog is lit.
//
// The game's fog (its vertex shaders, per vertex; g_commonVertexConstants.fogDistance = (b, a, -, c), here fog.xyz =
// (a, b, c)): between the camera and a point v away, tau = a |v| (1 - e^(-b v.y)) / (b v.y) (a |v| within 1 cm of the
// camera's height), its transmittance T = 2^-max(tau - c, 0), and its pixel shaders blend colour T + fogColor (1 - T):
// a height fog thinning upward, starting c into it, the same colour wherever it is. Here the same T (per pixel) and
// fogColor, but the light the fog scatters follows what reaches it: its share from the sun (fog_color.w) where the sun
// gets there, the rest where the sky does (fog_light). Fog that both reach is the game's: the fog's colour and density
// are the game's, its light shafts and its shadows the path tracer's.
#define TRACE_NO_TRACED
#include "trace_common.hlsli"

SamplerState linear_clamp : register(s0);

// x, y: froxels across the screen; z: slices in depth
uint3 froxel_count() { return uint3(uint(fog3.x), uint(fog3.y), uint(fog2.w)); }

// The view depth at slice coordinate s (0: the grid's near end fog2.y, slices fog2.w: its far end fog2.z), exponential:
// each slice as much deeper as the one before.
float slice_depth(float s) { return fog2.y * exp2(s / fog2.w * log2(fog2.z / fog2.y)); }
float depth_slice(float z) { return fog2.w * log2(max(z, 1e-4) / fog2.y) / log2(fog2.z / fog2.y); }

// The ray through the screen position uv (0 to 1, y down), its forward component 1: the point at view depth z is
// cam_origin + r z.
float3 view_ray_uv(float2 uv) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

// The light a froxel scatters toward the camera, over the game's fog's (1: as much), from how much of the sun and of the
// sky reaches it; dir: from the camera toward it (unit). The sun's share is weighted by the scattering's forward peak
// (Henyey-Greenstein, g = fog2.x: more of it toward the sun), so that its shadows stand out more looking that way; fog
// that the sun and the sky both reach gives 1 in every direction.
float fog_light(float vis_sun, float vis_sky, float3 dir) {
    if (sun.w == 0.0 || fog_color.w <= 0.0) return vis_sky;
    const float share = min(fog_color.w, 1.0);
    const float g = fog2.x;
    const float c = dot(dir, normalize(sun.xyz));
    const float p = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
    return (share * p * vis_sun + (1.0 - share) * vis_sky) / (share * p + (1.0 - share));
}
