#pragma once
#include <cstdint>
#include <vector>

struct Config;
struct ID3D11ShaderResourceView;
namespace rfgrt_frame {
struct SlotHeader;
}

// Shadows for the dynamic lights (research/docs/04 section 6.1). The game draws each visible point or spot light
// (rl_scene_renderer::m_visible_lights) in its own additive pass, render_lighting_passes, whose shaders multiply by
// ClbTexture.G: the sun's shadow, left bound at t15 (the 93 light-pass shaders that did not read it are patched to,
// patch/light_pass.h). Here the lights the scene renderer last drew, nearest first, get a shadow mask each, traced by
// the core from the frame scene (rtcore light_shadow.hlsl); for each light's pass its mask goes to t15, or a white one
// for a light without a mask, so no local light is darkened by the sun's shadow any more. A light keeps its mask
// while it stays in the list; one new to it waits a frame for its mask.
namespace lights {

bool install(const Config& cfg);
bool enabled();
// rl_scene_renderer::render's start (render thread, before the frame scene's worker starts): the frame's lights, their
// masks, from the scene renderer's visible lights (the previous frame's: this frame's are found later in render()).
void collect(uint8_t* scene_renderer, uint8_t* scene, uint8_t* camera);
// The frame scene's worker: the lights collected for this frame into its slot.
void write(rfgrt_frame::SlotHeader* h);
// The seam: whether the core answered this frame (its masks hold this frame's lights), and runs at all.
void on_seam(bool core_answered, bool core_ok);
// rl_scene_renderer::set_shadow_states, after the game bound ClbTexture: what t15 gets instead for a light's pass
// (nullptr: leave it).
ID3D11ShaderResourceView* t15_override();
// From the shader hooks: the light-pass patch (false: create the shader as it is).
bool patch_pixel_shader(const void* code, size_t size, std::vector<uint8_t>* out);
void report(bool at_exit);

}  // namespace lights
