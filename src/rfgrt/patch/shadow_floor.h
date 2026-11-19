#pragma once
// The sun's specular in shadow (research/docs/04 section 6.1). The game's lit shaders scale the sun's specular by
// ClbTexture's shadow channel with a floor, max(shadow, 0.05): a surface in full shadow keeps 5 % of its sun highlight,
// a stand-in for what it would reflect of its surroundings. Next to the game's flat ambient that went unnoticed; with the
// path-traced light, much darker indoors, those highlights shine inside buildings, containers and vehicles.
//
// The patch: where the core traced the pixel (the irradiance at t14 has alpha != 0), the floor is `in_shadow` (0 by
// default) instead; elsewhere (the core not answering, F11's vanilla view, the sky) it stays the game's 0.05:
//   ftoi rN.xy, v<pos>.xyxx
//   mov  rN.zw, l(0, 0, 0, 0)
//   ld   rN.xyzw, rN.xyzw, t14.xyzw
//   movc rN.x, rN.wwww, l(in_shadow), l(0.05)
//   max  <dst>, <the t15 sample>.y, rN.x        (was: max <dst>, <the t15 sample>.y, l(0.05))
// rfg-eosglass's glow (its cubemap times the sun's colour, mad <shadow>, 0.7, 0.3: 30 % of it in full shadow, glass that
// lit itself in the shade and lit its surroundings through the GI) the same way: weight 1 and floor 0 where traced. And the
// sun's terms that never meet the shadow at all (a `mul` by the sun's colour, cb1[0], whose other factor is not shadowed
// and whose result is not multiplied by a shadowed value later: the game's own eosglass variants' glow, the skin's) take
// the light that reaches the surface where traced: the sun's colour times ClbTexture's G read at the pixel, plus the
// traced irradiance (lit by the bounce in the shade, dark only where no light comes); the sun's colour elsewhere.
// t14 is declared if the shader does not read it yet (it is bound for every lit pass with the irradiance). Applies to the
// original shader or to one the GI and reflections' patches have rewritten. tools/shaderpatch checks it on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace shadow_floor {

enum class Result {
    NotPixelShader,
    NotLit,        // no ClbTexture (t15)
    NoFloor,       // lit, without the floor on t15's shadow channel
    Patched,
    Unsupported,   // `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
    uint32_t index = 0;  // the floor's instruction, among the program's
    uint32_t sun_terms = 0;  // the sun's terms that never met the shadow, made to
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, float in_shadow, std::vector<uint8_t>* out, Report* report);

}  // namespace shadow_floor
