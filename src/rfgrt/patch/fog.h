#pragma once
// The game's fog, left out where the volumetric fog takes its place. The game's vertex shaders compute the fog per
// vertex, f = 1 - min(2^-tau, 1) with tau = a d (1 - e^(-b dy)) / (b dy) - c (g_commonVertexConstants.fogDistance =
// (b, a, -, c); d the distance, dy the height above the camera), into an interpolator's component that the pixel
// shaders read in two places only: `add rA, -v.c, l(1)` (1 - f), then a lit pass's `mad o, v.cccc, cb0[4].xyzx,
// colour * (1 - f)` (fogColor), a light's additive pass just `light * (1 - f)`.
//
// The core's volumetric fog (rtcore/shaders/fog_apply.hlsl) is in a texture the plugin binds at t18 from the frame's
// first lit pass to the end of its lighting passes, where it blends it over the scene (render/gi.h): alpha > 0 where the
// fog is the core's (its transmittance), 0 where not (the sky, the landmark layer). Nothing is bound there for what is
// drawn after (the transparents keep the game's fog), with F11's vanilla view or without the core. The patch:
//   ftoi rN.xy, v<pos>.xyxx       (the pixel position, declared if the shader did not read it)
//   mov  rN.zw, l(0, 0, 0, 0)
//   ld   rN.w, rN.xyzw, t18.xyzw
//   lt   rN.w, l(0), rN.w
//   movc rN.x, rN.w, l(0), v<fog>.<c>
// and the fog's reads read rN.x instead: 0 where the core's fog is there, the game's elsewhere. A shader whose fog
// component is read anywhere else is left alone. tools/shaderpatch checks it on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace fog {

constexpr uint32_t kFogSlot = 18;  // t18: free in every shader of the game

enum class Result {
    NotPixelShader,
    NoFog,        // no fog of the game's in it
    Patched,
    Unsupported,  // fog, but the rewrite failed: `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
    uint32_t input = 0, component = 0;  // the fog's input register and component
    uint32_t reads = 0;                 // the reads rewritten (the `add`s and the `mad`s with the fog's colour)
    bool colour = false;                // the fog's colour added (a lit pass), else only the light's dimming
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace fog
