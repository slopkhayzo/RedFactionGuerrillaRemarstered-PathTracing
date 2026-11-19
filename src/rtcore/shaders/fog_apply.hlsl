// The volumetric fog, pass 3 of 3: per pixel, the fog in front of its opaque surface, into the texture the plugin
// blends over the lit scene at the end of the game's lighting passes (render/gi.cpp, on_lit), whose lit shaders leave
// their own fog out where this one is there (patch/fog.h):
//   rgb: the light the fog scatters toward the camera, fogColor R (1 - T); alpha: T, the game's transmittance to the
//   surface (per pixel here, per vertex in the game), at least 1e-4
// R read between froxels at the surface's depth (fog_integrate.hlsl); beyond the grid's far end the fog is taken as lit
// as in the open. 0 where the main scene has no surface (the sky, the landmark layer): the game's fog stays there.
#include "fog_common.hlsli"
Texture3D<float> fog_ratio : register(t3);
RWTexture2D<float4> fog_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float3 p;
    if (!position_at(int2(id.xy), p)) {
        fog_out[id.xy] = 0.0;
        return;
    }
    const uint3 n = froxel_count();
    const float3 v = p - cam_origin.xyz;
    const float z = dot(v, cam_forward.xyz);
    const float t = fog_transmittance(v);
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    float s;
    if (z <= fog2.z) {
        s = fog_ratio.SampleLevel(linear_clamp, float3(uv, (depth_slice(z) - 0.5) / float(n.z)), 0) * (1.0 - t);
    } else {
        const float tf = fog_transmittance(v * (fog2.z / z));
        s = fog_ratio.SampleLevel(linear_clamp, float3(uv, 1.0 - 0.5 / float(n.z)), 0) * (1.0 - tf) + max(tf - t, 0.0);
    }
    fog_out[id.xy] = float4(fog_color.rgb * max(s, 0.0), max(t, 1e-4));
}
