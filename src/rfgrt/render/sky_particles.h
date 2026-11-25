#pragma once
// Particles.AlwaysOnTopWithSky: while path tracing, the particles the game draws over everything are drawn with the sky
// instead, behind every world object. An always-on-top material (rl_particle_material_data::flags 0x80, from its
// emitter's is_always_on_top) makes rl_particle_renderer::render draw it without a depth test; the one effect that has
// it is the camera's wind dust (env_dust_camera_a.effectx: its ground, roof and upwind emitters), which then blew through
// walls and roofs and showed bright in the path tracer's dark interiors.
//
// For such an emitter the hook clears the flag for the call (the game's particle depth state then: tested, LessEqual,
// not written) and sets the renderer's viewport depth range to [1, 1]: every fragment lies at the far plane, so it is
// drawn only where nothing but the sky is (the depth there 1, the cleared value; landmarks sit in the range just below
// it). The soft fade keeps working (it compares view depths, not the viewport's). Both are restored after the call:
// the flag is read nowhere else, and the particles are drawn on one thread.
#include "core/config.h"

namespace sky_particles {

bool install(const Config& cfg);
void report(bool at_exit);

}  // namespace sky_particles
