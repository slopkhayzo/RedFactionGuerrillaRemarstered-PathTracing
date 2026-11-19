#pragma once
#include <cstdint>

struct Config;

// The render instances (research/docs/03, phase 2 step 2): rl_scene::add_renderable_instance and
// remove_renderable_instance keep a registry of which instances exist in which scene (the game defers both while the
// renderer is locked and replays them through the same functions, so the hooks see every real change). Once per frame,
// at the start of rl_scene_renderer::render on the render thread, snapshot() writes the frame's instances (mesh id,
// submesh, transform relative to the render offset) and camera into the core's frame slot (frame_scene.h).
namespace instances {

bool install(const Config& cfg);
// Render thread, at rl_scene_renderer::render's start, with its scene and camera.
void snapshot(uint8_t* renderer, uint8_t* scene, uint8_t* camera, int64_t frame);
void report(bool at_exit);

}  // namespace instances
