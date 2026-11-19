#pragma once
#include <cstdint>
#include <vector>

struct Config;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

// Global illumination through the game's own shading (research/docs/04 section 6.1). The lit base-pass pixel shaders
// are patched as the game creates them (patch/ambient.h) to read their ambient colour from t14, and t14 is bound to
// the irradiance texture wherever the game binds ClbTexture for a lit pass (rl_scene_renderer::set_shadow_states).
//
// GI.Debug fills the irradiance texture in the plugin, for checks: 1 magenta (which surfaces read it: every lit one
// should), 2 the game's ambient colour (the frame should look as the game's, but for the ambient's N.up variation).
namespace gi {

bool install(const Config& cfg);
bool enabled();
// rl_scene_renderer::render's start: the renderer and the scene of the frame.
void on_render(uint8_t* renderer, uint8_t* scene);
// The seam (render thread, before the scene thread's lit passes): this frame's irradiance texture.
// `core_irradiance`: the core's (GI.Debug=0 with the core running), else the plugin's fallback (the game's ambient).
// `core_reflection`: the core's reflections (t16), else t16 gets the irradiance (the sky, roughly, along any direction).
// `core_glass`, `core_glass_refl`: the light of the glass (t17) and what the clear glass reflects (t19), for the patched
// glass shader (patch/glass.h).
// `core_fog`: the core's volumetric fog (Fog.Enabled), bound at t18 for the frame's lit passes and blended over the
// scene at their end; null: the game's own fog.
void on_seam(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t width, uint32_t height, ID3D11ShaderResourceView* core_irradiance,
             ID3D11ShaderResourceView* core_reflection, ID3D11ShaderResourceView* core_glass,
             ID3D11ShaderResourceView* core_glass_refl, ID3D11ShaderResourceView* core_fog, ID3D11ShaderResourceView* core_sun_tint);
// rl_scene_renderer::render_lighting_passes' end (scene thread): the lit opaque scene, copied for the core's GI rays;
// then the volumetric fog over it (t18 unbound: what is drawn after keeps the game's fog).
void on_lit(uint8_t* scene_renderer, uint8_t* renderer);
// From the shader hooks: the patched bytecode of a lit base pass (false: create it as it is).
bool patch_pixel_shader(const void* code, size_t size, std::vector<uint8_t>* out);
// The lighting constants the main scene's lit passes draw with: g_commonFragmentConstants' ambient (float 8), back
// ambient (12) and fogColor (16), g_commonVertexConstants' fogDistance (float 52), from the scene thread's renderer at
// its lit passes (set_shadow_states; the last frame's). render()'s own renderer holds its depth pre-pass's and shadow
// cascades' values part of the time (the fog off: density 0, falloff 1): read beside them, a frame now and then went
// without fog. False until a lit pass has been seen.
struct LitConstants {
    float ambient[4], back_ambient[4], fog_color[4], fog_distance[4];
};
bool lit_constants(LitConstants* out);
// From the shader hooks, last, on the shader as patched so far (or the original): the game's fog left out where the
// core's is (patch/fog.h). False: no change.
bool patch_fog(const void* code, size_t size, std::vector<uint8_t>* out);
void report(bool at_exit);

}  // namespace gi
