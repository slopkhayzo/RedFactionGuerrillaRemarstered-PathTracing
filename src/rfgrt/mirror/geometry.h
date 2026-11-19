#pragma once
#include <cstdint>
#include <mutex>

struct Config;

// The renderer's meshes: keen::graphics::initializeRenderGeometry creates every one and freeRenderGeometry releases
// it (research/docs/01 section 5). Two users feed from these hooks:
//   - the census (Telemetry.GeometryCensus): what the game creates, by creator and vertex layout;
//   - the scene mirror (Mirror.Enabled, with the core): each mesh, decoded while its CPU data still exists (positions
//     as float3, triangle lists, rl_mesh's submesh table), goes to the core through the scene ring, which turns it
//     into bottom-level acceleration structures; a release removes it there.
namespace geometry {

bool install(const Config& cfg);
// The mirror's counters (and the census's), every 15 s and at the end.
void report(bool at_exit);

// The scene ring's id of a mesh (its keen::RenderGeometry), 0 when the mirror does not have it. Hold lock_ids()
// around a series of lookups.
std::unique_lock<std::mutex> lock_ids();
uint32_t mesh_id(const std::unique_lock<std::mutex>& held, const uint8_t* render_geometry);

// The first vertices of a few skinned meshes as sent (bind pose, bone indices, weights), for the snapshot's one-time
// check of the bone palette's convention. False when the mesh has none kept.
struct SkinSample {
    uint32_t count = 0;
    float pos[16][3];
    uint8_t bone[16][4];
    uint8_t weight[16][4];
};
bool skin_sample(uint32_t mesh_id, SkinSample* out);

// Terrain. A subzone's height mesh gets its chunk table (25 chunks) only after it is made, and neighbouring subzones
// overlap: where they do, the game draws one's chunks and not the other's. Sent whole, the parts not drawn shadowed the
// ground drawn there (bands along the slopes). So the mirror keeps a height mesh's data until its table is there, then
// sends it a submesh a chunk, and the snapshot puts in the chunks the game draws, each its own instance. For a height
// mesh instance's mesh: its chunks are sent (Chunks: put this chunk in), it went whole (Whole: once a mesh), or not yet
// (Pending: nothing yet). Under lock_ids(); `mesh` is the rl_mesh; sends the chunks once its table is there.
enum class Terrain { Chunks, Whole, Pending };
Terrain terrain(const std::unique_lock<std::mutex>& held, uint32_t mesh_id, uint8_t* mesh);

}  // namespace geometry
