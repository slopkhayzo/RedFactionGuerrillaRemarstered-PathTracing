#pragma once
// The game's eye adaptation in a dark scene (Tonemap.EyeAdaptationDark; research/docs/04 of OpenRFG64 VOM, section 6).
// rl_hdr's adaptation (its 06_ps: the adapted scene luminance A, the tone map's exposure k = Lum_range / (1 + Lum_offset
// x L)) and the bright pass's (10_ps, 18_ps: HDR_level / L) take
//   L = Eye_adaption_amount x (A - Eye_adaption_base) + Eye_adaption_base
// with the time of day's amount (0.3 to 0.5): the eye follows a scene only part of the way. Lit by its flat fill, the
// game's scenes stay near the base; path traced, a room behind a small opening is 10 to 30 times darker (A 0.01-0.04
// against the base 0.2) and the eye opened 1.7 times where it would open 3.
//
// The patch: below the base the scene adapts by `dark` instead; above it the game's amount stays (daylight keeps its
// look, and both meet at the base: L is continuous there). Before each such
//   mad rL.c, <amount>, <A - base>, <base>
// (c one component; the amount and the base read from the constant buffer by name):
//   lt   rT.c, <A - base>, l(0, 0, 0, 0)
//   movc rT.c, rT.c, l(dark, dark, dark, dark), <amount>
// and the mad reads rT.c for the amount (a new temp).
#include <cstdint>
#include <string>
#include <vector>

namespace eye_adaption {

enum class Result {
    NotPixelShader,
    NotAdaptation,  // no Eye_adaption_amount and Eye_adaption_base, or no mad of them
    Patched,
    Unsupported,    // `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
    uint32_t mads = 0;  // the adaptation's mads rewritten
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, float dark, std::vector<uint8_t>* out, Report* report = nullptr);

}  // namespace eye_adaption
