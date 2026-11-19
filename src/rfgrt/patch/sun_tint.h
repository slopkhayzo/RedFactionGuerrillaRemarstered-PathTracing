#pragma once
// The sun's light through clear glass (GI.WindowTint; research/docs/04 section 6.1). The core traces, for each pixel, what
// of the sun's light the clear panes between its surface and the sun let through, at the texel each ray crosses
// (src/rtcore/shaders/sun_tint.hlsl), into a texture the plugin binds at t20 for the lit passes: rgb that colour, alpha 1
// where traced.
//
// The patch: every read of the sun's colour (cb1[0], g_lightingFragmentConstants.lightColor) reads it times that colour
// where traced, the game's own elsewhere (the core not answering, F11's view):
//   ftoi rT.xy, v<pos>.xyxx
//   mov  rT.zw, l(0, 0, 0, 0)
//   ld   rT.xyzw, rT.xyzw, t20.xyzw
//   mul  rS.xyz, cb1[0].xyzx, rT.xyzx
//   movc rS.xyz, rT.wwww, rS.xyzx, cb1[0].xyzx
//   mov  rS.w, cb1[0].w
// before the first instruction that reads cb1[0]; each of them then reads rS instead (same components, same modifiers).
// For the base lit passes the GI's patch rewrote, on the shader as the patches before left it (the shadow floor's terms
// read the sun's colour too, and take the tint so); never a light's pass, whose cb1[0] is that light's colour.
// tools/shaderpatch checks it on WARP: t20's alpha 0 or white leaves the output as it was; a colour gives what the shader
// gives with the sun's colour times that colour.
#include <cstdint>
#include <string>
#include <vector>

namespace sun_tint {

constexpr uint32_t kSunTintSlot = 20;

enum class Result {
    NotPixelShader,
    NoSun,        // no lighting constants at cb1, no ClbTexture, or no read of the sun's colour
    Patched,
    Unsupported,  // `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
    uint32_t reads = 0;  // the instructions that read the sun's colour
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace sun_tint
