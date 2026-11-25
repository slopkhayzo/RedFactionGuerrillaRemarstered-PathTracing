#pragma once

struct Config;

// Telemetry.DumpShaders: KAIKO's layer loads its own shaders by name (the terrain's heightmesh_*.vso and the like),
// from an archive packed into rfg.exe. This saves the bytecode of every one as the game creates it, into
// <run folder>\shaders\<name>.dxbc, for fxc /dumpbin. Volition's effects (.fxo_kg) are in misc.vpp_pc instead.
//
// The same hooks serve the shader patches (GI's and the lights': render/gi.h, render/lights.h): a pixel shader is
// patched as the game creates it. For a game that switches path tracing (core/active.h) both are made, the original and
// the patched one: the game's shader object holds the one for path tracing on or off (use_patched), and the other is
// released with it (the game's seam "openrfg_release_pixel_shader").
namespace shader_dump {

bool install(const Config& cfg);
// The game's patched pixel shaders swapped for their originals or back (a game that switches path tracing).
void use_patched(bool on);

}  // namespace shader_dump
