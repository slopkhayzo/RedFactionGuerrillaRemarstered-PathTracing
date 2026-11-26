#pragma once
// The presenter (research plan 03, phase 2): the game's finished frames presented through a Direct3D 12 flip-model
// swap chain of its own, on a child window covering the game's client area, in place of the present of the game's
// Direct3D 11 swap chain (IDXGISwapChain::Present hooked: a seam in OpenRFG64 VOM, a detour on the game's chain in the
// plugins; the game's chain keeps everything else: its back buffer, resizes, full-screen switches). DXGI allows no
// second swap chain on the game's window, hence the child, made on the game window's own thread (disabled: the mouse
// and the keyboard still go to the game's window). It is the base of DLSS Frame Generation, which works on Direct3D 12
// swap chains only. Its Direct3D 12 device is its own, on the game's adapter, whatever the renderer. Exclusive full
// screen falls back to the game's chain. Ported from the SpaceEngineers-RTHWAccel project's presenter. 64-bit builds
// only (Streamline is 64-bit): the 32-bit plugin has none of it.

struct Config;

namespace presenter {

// At start-up: the Present hook. On OpenRFG64 VOM's seam always (its FRAME GENERATION row switches while playing); in
// the plugins when the presenter is wanted at start (FrameGeneration.Enabled or .Presenter) or `switchable` (the display
// menu's FRAME GENERATION row: menu/display_rows.h), as a detour on the game's swap chain placed at its first frame's
// end (off, it passes every present through). False when it cannot be hooked.
bool install(const Config& cfg, bool switchable = false);
// Frame generation wanted or not (rfgrt_set_frame_generation; FrameGeneration.Enabled at start): from the next present.
// Without it (and without FrameGeneration.Presenter) the game's swap chain presents again.
void set_wanted(bool on);
// The plugins' scene's end: the main scene's rl_scene_renderer::render returned, its commands run (the frame drawn, the
// HUD not yet): the frame before the HUD, the depth and the camera taken for frame generation, as OpenRFG64 VOM's seam
// after the scene does (there this does nothing). Render thread.
void scene_rendered(void* scene_renderer, void* camera);

}  // namespace presenter
