#pragma once
// The global illumination patch (research/docs/04 section 6.1): a lit base-pass pixel shader's diffuse ambient, the
// game's hemispherical ambient (g_commonFragmentConstants ambient/backAmbient, cb0[2] and cb0[3], blended by N.up) with
// its fake bounce (lightColor * indirectLightParams), is replaced by the path-traced irradiance the core writes for
// each pixel, read from t14. Everything the shader does with it afterwards (ambient occlusion from ClbTexture.R,
// self-illumination, the albedo) stays as it was, and so does its specular ambient.
//
// The ambient is found by dataflow, as the compiler orders it differently in different shaders (ambient.cpp): the latest
// value made from those constants and the normal alone that every path to the albedo passes through. tools/shaderpatch
// checks the result on the game's shaders by running each against its original on WARP.
#include <cstdint>
#include <string>
#include <vector>

namespace ambient {

constexpr uint32_t kIrradianceSlot = 14;  // t14: free in every lit shader of the game (they use t0-t12, t13, t15)
constexpr uint32_t kReflectionSlot = 16;  // t16: as free (a pixel shader has 128 slots)

enum class Result {
    NotPixelShader,
    NotLit,         // no ClbTexture (t15): not a lit pass
    NoAmbient,      // lit, without the ambient constants (the dynamic lights' additive passes)
    Patched,
    Unsupported,    // has the ambient, but the analysis or the rewrite failed: `reason` says why
};
struct Report {
    Result result = Result::Unsupported;
    std::string reason;
    std::string ambient;           // the instruction whose result is the ambient colour
    uint32_t ambient_index = 0;    // its place among the instructions
    uint32_t temps_before = 0;
    bool loose = false;            // found only with scalar weights from textures inside it (foliage, glowing materials)
};
const char* name(Result r);

// Patches the shader if it is a lit base pass; `out` receives the new container (with its checksum) when Patched.
Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);
// The reflections' patch: the specular ambient (the same sky looked up along the reflection vector, with the fake
// bounce, before the specular map, the fresnel and indirectLightParams.z apply) read from t16 instead, by the same
// analysis aimed at the specular uses. Applies to a shader the GI patch has already rewritten as well.
Result patch_reflection(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report);

}  // namespace ambient
