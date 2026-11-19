#pragma once
// The sky's reflection in the shade, where the reflections' patch could not go (research/docs/04 section 6.1). The game's
// lit shaders reflect a sky colour (its ambient looked up along the reflection vector) scaled by indirectLightParams.z
// (cb0[7].z) and darkened by ClbTexture's ambient occlusion. With the path-traced GI that occlusion is 1 (the traced
// light holds it), so a shader whose specular ambient could not be replaced by the traced reflections (the terrain's,
// patch/ambient.h: "no single specular ambient value") reflected the open sky in caves, corridors and under buildings.
// The patch scales cb0[7].z by how much of the sky's light reaches the pixel, from the traced irradiance at t14:
//   k = saturate(luminance(irradiance) / luminance(ambient, cb0[2]))   (1 in the open, toward 0 in the dark)
// where the core traced the pixel (alpha != 0); elsewhere (F11's vanilla view, the core not answering) 1, the game's.
// Every read of cb0[7].z (the specular ambient's scale, its only use) reads the scaled value. Applies to a shader the GI
// patch has rewritten (t14 declared). tools/shaderpatch checks it on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace spec_occlusion {

enum class Result { NotPixelShader, NoSpecularAmbient, Patched, Unsupported };
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
};
const char* name(Result r);

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace spec_occlusion
