#pragma once
#include <cstdint>

struct Config;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

// The frame seam (research/docs/01 section 3): rl_scene_renderer::render, on the render thread, records the depth
// pre-pass, the occlusion queries and the shadow cascades with their projection into ClbTexture straight onto the
// immediate context, then finishes the scene thread's command list (keen::graphics::endCommandBuffer, called only
// there) and executes it: SSAO, the base pass that samples ClbTexture, everything after. Right after
// endCommandBuffer the depth and ClbTexture of this frame are complete on the immediate context and nothing has read
// ClbTexture yet: that is where ray-traced results go in.
namespace seam {

bool install(const Config& cfg);
// Stops the ray tracing core (session flush handler).
void shutdown(bool at_exit);
// The frame number of the render() under way, for the scene thread that records the same frame's passes (its
// command list ends at the seam): 0 before frames are exchanged with the core.
int64_t frame_rendering();
// The main scene's rl_scene_renderer, once a seam has seen it (null before). The scene thread records its passes with
// an rl_renderer of its own, not the one render() is given: the scene renderer is what tells the main scene's passes.
uint8_t* main_scene_renderer();
// The core's volumetric fog (rgb what it scatters toward the camera, a what of the surface's light gets through; a 0
// where the fog is not the core's) blended over the lit scene in `target` (w x h): the scene's colour times a plus rgb.
// Recorded on `ctx` (the scene thread's), its state put back after.
void composite_fog(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, uint32_t w, uint32_t h, ID3D11ShaderResourceView* fog);

}  // namespace seam
