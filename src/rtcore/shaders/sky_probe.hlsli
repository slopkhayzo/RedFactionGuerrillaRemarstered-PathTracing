#pragma once
// The sky as the game draws it, by direction: what a ray that gets out sees. The game lights its shade with an ambient
// colour of its own (a bluish one by day on Mars), not with the sky it draws (red to orange), so a door or a window let
// in that ambient colour, not the sky behind them. sky_update.hlsl learns the drawn sky from the pixels that show it
// (the previous frame's colour where neither frame had a surface: the far plane, not the landmark layer), away from the
// sun's disk and glow (its light comes as the sun's, sun_at), into an octahedral map of the whole sphere (32 x 32
// texels of about 6 degrees); sky_resolve.hlsl averages it over frames and keeps the mean of what it saw for the
// directions not seen yet. Below the horizon, and before any sky was seen, the game's own ambient colours stay.
//
// The sky the game draws is far darker than its ambient colour (about 0.1 against 0.4 in its shaders' units by day: it
// lit its shade brighter than its sky would), so GI.SkyDrawn (sky_probe.z) has three settings: 0 the game's ambient
// colour; 1 the drawn sky's colour (each direction's, relative to the others) at the brightness of the game's ambient;
// 2 the drawn sky as it is; values between blend.
RWStructuredBuffer<uint> sky_accum : register(u4);    // per texel: r, g, b sums (1/256 units), count
RWStructuredBuffer<float4> sky_value : register(u5);  // per texel: rgb the radiance, a the frame it was last seen plus
                                                      // one (asuint; 0: not seen in the last half minute); [kSkyTexels]:
                                                      // the mean of those seen (a 1)

static const uint kSkyRes = 32u;
static const uint kSkyTexels = kSkyRes * kSkyRes;

// Octahedral mapping of a unit direction to [0, 1]^2 (y up).
float2 sky_oct(float3 d) {
    const float3 a = abs(d);
    float2 p = d.xz / max(a.x + a.y + a.z, 1e-6);
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}

uint sky_texel(float3 d) {
    const uint2 t = min(uint2(sky_oct(d) * float(kSkyRes)), uint2(kSkyRes - 1u, kSkyRes - 1u));
    return t.y * kSkyRes + t.x;
}

// The sky's radiance along dir (the game's units over pi): the game's ambient model (`model`, its value straight up
// being `model_up`) toward the drawn sky, by sky_probe.z (GI.SkyDrawn, above), where some sky has been seen.
float3 sky_drawn(float3 dir, float3 model, float3 model_up) {
    if (sky_probe.x == 0.0 || dir.y < 0.0 || sky_probe.z <= 0.0) return model;
    const float4 mean = sky_value[kSkyTexels];
    if (mean.a == 0.0) return model;
    float4 v = sky_value[sky_texel(dir)];
    const float3 drawn = asuint(v.a) != 0u ? v.rgb : mean.rgb;
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    const float scale = dot(model_up, lw) / max(dot(mean.rgb, lw), 1e-6);  // the drawn sky at the game's brightness
    const float3 tinted = drawn * scale;
    return sky_probe.z <= 1.0 ? lerp(model, tinted, sky_probe.z) : lerp(tinted, drawn, saturate(sky_probe.z - 1.0));
}
