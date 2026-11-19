// The volumetric fog, pass 2 of 3: front to back along each column of froxels, the light the fog scatters toward the
// camera up to each slice's far end, over what the game's fog scatters up to there (fogColor (1 - T)): the ratio R
// (fog_light's, weighted by how much of the fog each slice holds), so that the light of the fog in front of a surface
// is fogColor R (1 - T), T the game's transmittance to it (fog_apply.hlsl). R is 1 where the sun and the sky reach all
// of the fog on the way (the game's fog), less where they do not; never more.
//   u0 (R16F): R at each slice's far end
#include "fog_common.hlsli"
Texture3D<float4> fog_vis : register(t3);
RWTexture3D<float> fog_ratio : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint3 n = froxel_count();
    if (any(id.xy >= n.xy)) return;
    const float3 r = view_ray_uv((float2(id.xy) + 0.5) / float2(n.xy));
    const float3 dir = normalize(r);
    float before = 1.0, s = 0.0;
    for (uint k = 0; k < n.z; ++k) {
        const float t = fog_transmittance(r * slice_depth(float(k + 1)));
        const float4 v = fog_vis[uint3(id.xy, k)];
        const float m = fog_light(v.x, v.y, dir);
        s += m * max(before - t, 0.0);
        before = t;
        fog_ratio[uint3(id.xy, k)] = 1.0 - t > 1e-5 ? saturate(s / (1.0 - t)) : m;
    }
}
