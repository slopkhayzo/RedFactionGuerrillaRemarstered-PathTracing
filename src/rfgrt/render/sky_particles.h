#pragma once
// While path tracing, the wind's particles are drawn with the sky, behind every world object.
//
// Particles.AlwaysOnTopWithSky: the particles the game draws over everything. An always-on-top material
// (rl_particle_material_data::flags 0x80, from its emitter's is_always_on_top) makes rl_particle_renderer::render draw
// it without a depth test; the one effect that has it is the camera's wind dust (env_dust_camera_a.effectx: its ground,
// roof and upwind emitters), which then blew through walls and roofs and showed bright in the path tracer's dark
// interiors.
// Particles.WithSkyEffects: the effects whose particles get the same, by the start of their name (default: the wind's
// gusts and dust devils, env_gust_a/b and env_gust_devil_a, which code_effects/dust.cpp plays near or far ahead of the
// player every 0.2 s, indoors too, and drift through walls). vfx_effect_template::init tags their emitters' materials
// (a flag bit the game never uses) as each template is made.
//
// For such an emitter, while path tracing, the hook draws it with the game's particle depth state (tested, LessEqual,
// not written: an always-on-top flag is cleared for the call) and the renderer's viewport depth range [1, 1]: every
// fragment lies at the far plane, so it is drawn only where nothing but the sky is (the depth there 1, the cleared
// value; the scene is drawn in [0, 0.9922], landmarks in the reserved range just below 1). The soft fade keeps working
// (it compares view depths, not the viewport's). Both are restored after the call: the flag is read nowhere else, and
// the particles are drawn on one thread.
#include "core/config.h"

namespace sky_particles {

bool install(const Config& cfg);
void report(bool at_exit);

}  // namespace sky_particles
