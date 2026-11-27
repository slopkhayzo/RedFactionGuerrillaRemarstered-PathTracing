#pragma once
// The rim light of the characters' cloth (rfg-brdf-cloth_rimlight; research/docs/04 section 13), while path tracing.
// The game's lit pass adds to a cloth's ambient a glow along its edges turned away from the sun:
//   rim = saturate((0.4 - N.V) * 2.5) * max(1 - 1.5 (0.5 N.L + 0.5), 0) * clamp(time.w, 0.25, 1) * 9
// (times the cloth's colour, as the ambient is): a light from behind, in proportion beside the game's flat ambient, but
// beside the path-traced light, darker in the shade, it glows. The traced light holds what reaches those edges from
// behind already. The patch scales the rim by `scale` (GI.RimLight: 0 none, 1 the game's) where the core traced the
// pixel (the irradiance's alpha at t14 not 0); elsewhere (F11's vanilla view, the core not answering) it is the game's.
// The rim is found by its dataflow: add r.c, -x, l(0.4); mul_sat r.c, r.c, l(2.5); then only products of r.c with other
// values, up to its product with an immediate (9), which then reads the scaled value. Applies to a shader the GI patch
// has rewritten (t14 declared). tools/shaderpatch checks it on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace rim_light {

enum class Result { NotPixelShader, NoRimLight, Patched, Unsupported };
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, float scale, std::vector<uint8_t>* out, Report* report);
// For tools/shaderpatch: the same shader with the rim's immediate factor times `scale` (wherever the core traced or not).
Result scaled(const void* bytes, size_t size, float scale, std::vector<uint8_t>* out, Report* report);

}  // namespace rim_light
