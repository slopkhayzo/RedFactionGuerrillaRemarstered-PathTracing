// The glass's light passes' packed alpha (glass_light.hlsl, glass_filter.hlsl): a glass sample's view depth, the sun's
// visibility there and the cosine of its angle, in the bits of one float. Bits 0-14: the depth in 1/16 m plus 1 (up to
// 2 km); 15-22: the visibility in 1/255; 23: always 1; 24-29: the cosine in 1/63; 30-31: 0. The float's exponent (bits
// 23-30) is then 1 to 127: a normal number whatever the values (never 0, a denormal, infinite or NaN), so that loads and
// stores keep it exactly. 0.0: no glass.

struct GlassSample {
    float t;       // the view depth (m)
    float vis;     // how much of the sun reaches the glass, 0 to 1
    float cosine;  // the cosine of the sun's angle to the glass
};

float glass_pack(float t, float vis, float cosine) {
    const uint tq = min(uint(round(max(t, 0.0) * 16.0)) + 1u, 32767u);
    const uint vq = uint(round(saturate(vis) * 255.0));
    const uint cq = uint(round(saturate(cosine) * 63.0));
    return asfloat(tq | (vq << 15) | (1u << 23) | (cq << 24));
}

bool glass_unpack(float a, out GlassSample g) {
    const uint u = asuint(a);
    g.t = float((u & 0x7FFFu)) / 16.0 - 1.0 / 16.0;
    g.vis = float((u >> 15) & 0xFFu) / 255.0;
    g.cosine = float((u >> 24) & 0x3Fu) / 63.0;
    return (u & (1u << 23)) != 0u && (u >> 30) == 0u;
}
