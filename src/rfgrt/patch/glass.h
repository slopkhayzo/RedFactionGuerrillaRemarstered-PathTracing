#pragma once
// The glass's light (research/docs/04 section 6.1). The game's plain glass (rfg-glass's main pass: windows, their
// shards) lights itself after the lit passes: its diffuse light is the game's ambient and back ambient with a fake
// bounce of the sun (lerp(backAmbient, lightColor * indirectLightParams.x, N.L / 2 + 1/2), toward the ambient at the
// horizon), and the sun's highlight comes with no shadow at all. The core traces the light at the nearest glass in front
// of each pixel (rtcore/shaders/glass_light.hlsl, glass_filter.hlsl) into the texture the plugin binds at t17: rgb the
// irradiance there in the game's units (the sun's included), alpha 1 + how much of the sun reaches the glass (0 to 1:
// soft shadows), 0 where there is no glass. The patch:
//   ftoi    rN.xy, v<pos>.xyxx       (the pixel position, declared if the shader did not read it)
//   mov     rN.zw, l(0, 0, 0, 0)
//   ld      rN.xyzw, rN.xyzw, t17.xyzw
//   add_sat rM.x, rN.w, l(-1)        (the sun's visibility)
//   movc    rM.x, rN.wwww, rM.xxxx, l(1)
//   after the highlight times lightColor:         mul  <highlight>, <highlight>, rM.xxxx
//   before the light meets the albedo's colour:   movc <light>, rN.wwww, rN.xyz, <light>
// And what the clear glass reflects (Reflections.Glass): the core traces the mirror direction off the glass and writes
// at t19 the radiance shown (the game's lit colour units) times Schlick's Fresnel term F, and F in alpha (0: no glass,
// no reflection). Every write of the output goes to a temp instead, and before each ret:
//   ftoi rR.xy, v<pos>.xyxx; mov rR.zw, l(0); ld rR.xyzw, rR.xyzw, t19.xyzw      (F L, F)
//   mad  rS.x, -rO.w, rR.w, rR.w;  add rS.x, rS.x, rO.w                           a' = a + F (1 - a)
//   max  rS.y, rS.x, l(1e-6)
//   mad  rR.xyz, rO.xyz, rO.www, rR.xyz;  div rR.xyz, rR.xyz, rS.yyy             (c a + F L) / a'
//   movc rO.xyz, rR.www, rR.xyz, rO.xyz;  movc rO.w, rR.w, rS.x, rO.w             only where there is a reflection
//   mov  o0, rO
// so that blended over the scene (SRC_ALPHA, INV_SRC_ALPHA: the glass's colour is not premultiplied) the glass shows its
// colour c a and the reflection F L over (1 - a)(1 - F) of what is behind it: the reflection at its surface, not under
// its tint.
// Where alpha is 0 (no glass found, the core not answering, F11's vanilla view) the shader computes what the original
// does. Only a shader of that make is patched: the sun's colour (cb1[0]) met once by a `mul`, whose result is added to
// the light times the diffuse texture's colour, and no shadow (ClbTexture's G) read: rfg-glass (unlit) and
// rfg-glass-etched (it reads ClbTexture's occlusion only). tools/shaderpatch checks it on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace glass {

constexpr uint32_t kGlassSlot = 17;      // t17: free in every shader of the game
constexpr uint32_t kGlassReflSlot = 19;  // t19 (what the clear glass reflects): free too (t18: the fog's)

enum class Result {
    NotPixelShader,
    NotGlass,      // not a shader of that make
    Patched,
    Unsupported,   // of that make, but the rewrite failed: `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace glass
