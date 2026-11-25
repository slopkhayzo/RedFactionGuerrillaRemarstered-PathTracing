#pragma once
// The materials for the rays' hits (world-space shading): what a material map entry draws, as the core's shaders read it
// (scene_ring.h MaterialEntry: its diffuse map, tiling, colour, self-illumination), and the diffuse maps themselves: a
// game texture is copied, its mips from 256 texels down, into a texture shared with the core, which keeps it in its
// bindless table at the id given here (scene_ring.h kTextureAdd). Reflections and the GI's hits off screen then show the
// surface's own colour instead of a plain grey (mirror/instances.cpp sends each mesh's materials, kMeshMaterials).
#include <cstdint>

#include "scene_ring.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace textures {

constexpr uint32_t kNone = rfgrt_scene::kNoTexture;
enum State : uint8_t { kUnknown = 0, kPending = 1, kSent = 2, kFailed = 3 };

// The textures sent at all (GI.Textures, with the core's GI or reflections); off: materials name none.
void set_enabled(bool on);
bool enabled();
// The core's id for a game texture handle (rl_texture_desc's): the texture is copied and sent at a later seam. kNone
// for no texture, textures off, or past the table's end. Any thread.
uint32_t id_of(uint32_t handle);
// Where a texture id stands: kSent once the core has it (a material naming it may follow), kFailed for one that will never
// come (kNone included). Any thread.
State state(uint32_t id);
// A material (its handle in a material map's entries): its diffuse map (id_of), the constants the game's shaders take with
// it. False when it cannot be read (out: no texture, the defaults). Any thread the snapshot runs on.
bool material_entry(uint32_t material_handle, rfgrt_scene::MaterialEntry* out);
// Render thread, at the seam, before fence A's signal for `frame`: sends the copies whose frame the core has queued
// (`core_queued`: a frame it queues after reading the record waits on a later fence value, after the copy), then copies a
// few of the textures asked for since (their device objects are the game's: read there), sent at a later seam.
void on_seam(ID3D11Device* dev, ID3D11DeviceContext* ctx, int64_t frame, int64_t core_queued);
void log_stats();
// Path tracing switched off (core/active.h): the core's texture table is gone; the next core's starts empty, and the
// textures are asked for again as the materials name them.
void on_core_stop();

}  // namespace textures
