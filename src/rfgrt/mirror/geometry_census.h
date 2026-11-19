#pragma once
#include <cstdint>

// Phase 2 groundwork (research/docs/03, phase 2): logs what meshes the renderer creates, by creator (static meshes,
// Geo-Mod's compiled buildings, morphs, keen models, particles), vertex layout and position format, and what kind of
// D3D11 buffers hold them: what the ray tracing core has to decode. Telemetry.GeometryCensus=1; fed by the geometry
// hooks (mirror/geometry.h).
namespace geometry_census {

void enable();  // reports every 15 s from then on
void record(uint8_t* geometry, const char* creator);
void report(bool at_exit);  // what is new, and the session's total

}  // namespace geometry_census
