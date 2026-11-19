#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The game's light passes (research/docs/04 section 6.1): one additive pass per dynamic light (rl_light), shaded with
// g_lightingFragmentConstants (lightColor, attenuation, hotspot). Of its 200 light-pass pixel shaders, 107 multiply by
// ClbTexture.G at t15 (the sun's shadow, which the base pass leaves bound there) and 93 by nothing. Each light's shadow
// mask goes to t15 for its pass (render/lights.h), so the 93 are patched to read it as well: their colour output
// times t15.y.
namespace light_pass {

constexpr uint32_t kShadowSlot = 15;

enum class Result { NotPixelShader, NotLightPass, ReadsShadow, Patched, Unsupported };
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
};
const char* name(Result r);
// A light pass that does not read t15 is rewritten (`out`: the new container); ReadsShadow needs nothing.
Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace light_pass
