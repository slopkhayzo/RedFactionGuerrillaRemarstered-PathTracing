// The volumetric fog, pass 3 of 3: per pixel, the fog in front of its opaque surface, into the texture the plugin
// blends over the lit scene at the end of the game's lighting passes (render/gi.cpp, on_lit), whose lit shaders leave
// their own fog out where this one is there (patch/fog.h):
//   rgb: the light the fog scatters toward the camera, fogColor R (1 - T); alpha: T, the game's transmittance to the
//   surface (per pixel here, per vertex in the game), at least 1e-4
// R read between froxels at the surface's depth (fog_integrate.hlsl); beyond the grid's far end the fog is taken as lit
// as in the open. 0 where the main scene has no surface (the sky, the landmark layer): the game's fog stays there.
// And the same in front of the nearest clear glass (fog4.z): at the view depth the glass's light pass keeps for it
// (glass_light.hlsl, glass_raw's alpha at t4), into glass_fog (u7, the plugin's), which the patched glass shader reads
// at t21 and puts over the glass and what it reflects in place of the game's fog (patch/glass.h); 0 where no glass. The
// game drew that glass with its own fog: none within its start (Fog.Near), no shade, tinted by the glass's colour.
#include "fog_common.hlsli"
#include "glass_common.hlsli"
Texture3D<float> fog_ratio : register(t3);
Texture2D<float4> glass_raw : register(t4);
RWTexture2D<float4> fog_out : register(u0);
RWTexture2D<float4> glass_fog : register(u7);

// The fog between the camera and the point v away from it (on the ray through the screen position uv): rgb its light
// toward the camera, a its transmittance (at least 1e-4).
float4 fog_to(float3 v, float2 uv, uint3 n) {
    const float z = dot(v, cam_forward.xyz);
    const float t = fog_transmittance(v);
    float s;
    if (z <= fog2.z) {
        s = fog_ratio.SampleLevel(linear_clamp, float3(uv, (depth_slice(z) - 0.5) / float(n.z)), 0) * (1.0 - t);
    } else {
        const float tf = fog_transmittance(v * (fog2.z / z));
        s = fog_ratio.SampleLevel(linear_clamp, float3(uv, 1.0 - 0.5 / float(n.z)), 0) * (1.0 - tf) + max(tf - t, 0.0);
    }
    return float4(fog_color.rgb * max(s, 0.0), max(t, 1e-4));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const uint3 n = froxel_count();
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    if (fog4.z > 0.0) {
        GlassSample g;
        const bool glass = glass_unpack(glass_raw[id.xy].a, g) && g.t > 0.0;
        glass_fog[id.xy] = glass ? fog_to(view_ray_uv(uv) * g.t, uv, n) : float4(0.0, 0.0, 0.0, 0.0);
    }
    float3 p;
    fog_out[id.xy] = position_at(int2(id.xy), p) ? fog_to(p - cam_origin.xyz, uv, n) : float4(0.0, 0.0, 0.0, 0.0);
}
