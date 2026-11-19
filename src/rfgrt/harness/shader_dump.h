#pragma once

struct Config;

// Telemetry.DumpShaders: KAIKO's layer loads its own shaders by name (the terrain's heightmesh_*.vso and the like),
// from an archive packed into rfg.exe. This saves the bytecode of every one as the game creates it, into
// <run folder>\shaders\<name>.dxbc, for fxc /dumpbin. Volition's effects (.fxo_kg) are in misc.vpp_pc instead.
namespace shader_dump {

bool install(const Config& cfg);

}  // namespace shader_dump
