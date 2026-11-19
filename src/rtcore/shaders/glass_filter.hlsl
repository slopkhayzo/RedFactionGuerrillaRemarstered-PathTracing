// The light of the glass, pass 2 of 2 (after glass_light.hlsl): the light from the sky and the surroundings and the sun's
// visibility it traced (a few rays a pixel, averaged over frames: still grainy) averaged again over the glass around each
// pixel, 25 pixels of a 9 x 9 window weighed by their distance on screen, only those with glass at about the same distance
// (another pane, or what is beyond the glass's edge, stays out); then the sun's light at this pixel added: its colour,
// times that visibility (soft shadows), times the cosine here. Into the texture the patched glass shader reads at t17
// (patch/glass.h):
//   rgb: the irradiance at the glass in the game's units; a: 1 + the sun's visibility there (0 to 1); 0: no glass in
//   front of the pixel.

#include "trace_common.hlsli"
#include "glass_common.hlsli"
Texture2D<float4> raw : register(t3);  // glass_light.hlsl's

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const float4 c = raw[id.xy];
    GlassSample g;
    if (!glass_unpack(c.a, g)) {
        traced[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float tolerance = 0.03 * g.t + 0.05;
    float3 sum = 0.0;
    float vis = 0.0, weight = 0.0;
    for (int y = -4; y <= 4; y += 2) {
        for (int x = -4; x <= 4; x += 2) {
            const int2 q = int2(id.xy) + int2(x, y);
            if (any(q < 0) || any(q >= int2(size))) continue;
            const float4 n = raw[q];
            GlassSample gn;
            if (!glass_unpack(n.a, gn) || abs(gn.t - g.t) > tolerance) continue;
            const float w = exp(-0.08 * float(x * x + y * y));
            sum += n.rgb * w;
            vis += gn.vis * w;
            weight += w;
        }
    }
    const float3 indirect = weight > 0.0 ? sum / weight : c.rgb;
    const float v = weight > 0.0 ? saturate(vis / weight) : g.vis;
    const float3 direct = sun.w != 0.0 ? sun_color.rgb * (v * g.cosine) : 0.0;
    traced[id.xy] = float4(max(indirect, 0.0) + direct, 1.0 + v);
}
